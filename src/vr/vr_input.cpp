// The VR controllers, as the keyboard and mouse the game is played with.
//
// The first version fed XInput. The game has a gamepad scheme, so looking like
// a pad seemed the shortest route, and it was wrong twice over.
//
// Structurally: a thumbstick maps deflection to a turn *rate*, capped by the
// game's sensitivity. It cannot flick the way a mouse can, and aim here is
// deliberately decoupled from the head so the controllers can drive it later.
// Routing that through a stick builds in exactly the limit you would then
// have to take out again.
//
// Practically: the game calls XInputGetState once at startup, is told no pad
// is connected -- because no action happened to be active in that instant --
// and never asks again. trlvr_input.log showed it plainly: runtime found,
// manifest accepted, every handle resolved, and the "after 300 polls" line
// never reached. A perfect gamepad mapping would have been polled once and
// abandoned.
//
// So this drives the keyboard and mouse the game is actually played with,
// through SendInput, from the d3d9 side -- which is guaranteed to be loaded
// and runs every frame. The game takes its keyboard through window messages
// (USER32 gives it only cursor functions, no GetAsyncKeyState) and its mouse
// through GetCursorPos with a recentre, and synthesised input reaches both.
//
// Keys are sent as scan codes, looked up from the virtual key on the current
// layout, so they round-trip to the same key the game expects on AZERTY as on
// QWERTY -- and anything that reads scan codes sees real ones.
//
// One action set stays active everywhere. In menus, the same physical inputs
// are translated to arrows, Enter and Escape. This avoids depending on a
// second SteamVR action set (which some runtimes left completely unbound).
// Menu state comes from the engine's UI screen stack at 0x00F16E90.
//
//     mov ecx,[0x00F16E90] / test ecx,ecx / setge al
//
// so knowing a menu is up needs no hook at all. The top screen's id is logged
// on every change, because if HUD elements turn out to live on the same stack
// the menu set would be live during play -- and the log would say so at once.
#include "vr_input.h"
#include "../proxy/window.h"

#include "camera_head.h"
#include "hud_capture.h"
#include "ui_space.h"
#include "tune.h"
#include "vr_gesture.h"
#include "vr_session.h"
#include "vr_submit.h"

#include "../common/config.h"
#include "../common/log.h"

#include <openvr.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <windows.h>

namespace trlvr
{
    namespace
    {
        enum Output { KEY, MOUSE_LEFT, RECENTRE, GEAR_TUNING };

        struct Button
        {
            const char*          path;
            Output               out;
            WORD                 vk;
            vr::VRActionHandle_t handle;
            bool                 was;      // for the edge-triggered recentre
        };

        Button g_game[] = {
            { "/actions/gameplay/in/jump",       KEY,        VK_SPACE,  0, false },
            { "/actions/gameplay/in/interact",   KEY,        'E',       0, false },
            { "/actions/gameplay/in/dive",       KEY,        'F',       0, false },
            { "/actions/gameplay/in/fire",       MOUSE_LEFT, 0,         0, false },
            { "/actions/gameplay/in/grapple",    KEY,        'Q',       0, false },
            { "/actions/gameplay/in/grenade",    KEY,        'K',       0, false },
            { "/actions/gameplay/in/aim",        KEY,        'Z',       0, false },
            { "/actions/gameplay/in/lockon",     KEY,        'G',       0, false },
            { "/actions/gameplay/in/weapon",     KEY,        VK_END,    0, false },
            { "/actions/gameplay/in/pause",      KEY,        VK_ESCAPE, 0, false },
            { "/actions/gameplay/in/medkit",     KEY,        VK_HOME,   0, false },
            { "/actions/gameplay/in/light",      KEY,        VK_DELETE, 0, false },
            { "/actions/gameplay/in/binoculars", KEY,        VK_NEXT,   0, false },
            { "/actions/gameplay/in/recentre",   RECENTRE,   0,         0, false },
            { "/actions/gameplay/in/gear_tuning", GEAR_TUNING, 0,       0, false },
            // The PDA (retail Tab, Input slot 11; tester report 2026-10-06:
            // it had no button). Right stick double-click, which developer
            // gear tuning had (F6 still opens that).
            { "/actions/gameplay/in/pda",        KEY,        VK_TAB,    0, false },
        };

        const int kGameButtons = (int)(sizeof(g_game) / sizeof(g_game[0]));
        enum GameButton
        {
            GameJump, GameInteract, GameDive, GameFire, GameGrapple,
            GameGrenade, GameAim, GameLockOn, GameWeapon, GamePause,
            GameMedkit, GameLight, GameBinoculars, GameRecentre,
            GameGearTuning, GamePda
        };

        vr::IVRInput*  g_input = nullptr;
        vr::IVRSystem* g_system = nullptr;

        vr::VRActionSetHandle_t g_set_game = vr::k_ulInvalidActionSetHandle;
        vr::VRActionHandle_t g_move = 0;
        vr::VRActionHandle_t g_look = 0;
        // Y held as a plain button (beside its click/long-press bindings):
        // the board-mode finger flick. The long-press "light" output only
        // pulses, so it could not hold the flick loaded (test 2026-10-03).
        vr::VRActionHandle_t g_flick_action = 0;
        vr::VRActionHandle_t g_handhold_haptic[2] = { 0, 0 };
        // Grip buttons as last sampled by vr_input_update (left, right).
        bool g_grip_held[2] = { false, false };
        // Snap turning: the right stick must return near centre between steps.
        bool g_snap_turn_armed = true;
        // Physical crouch holding the crouch key, and when it last ended.
        bool g_physical_crouch = false;
        ULONGLONG g_physical_stand_at = 0;
        // Physical triggers (left, right), as last sampled in gameplay.
        bool g_trigger_held[2] = { false, false };
        // /actions/gameplay/in/hand_left|right, bound to /pose/grip.
        vr::VRActionHandle_t g_hand_pose[2] = { 0, 0 };
        unsigned g_grip_samples[2] = { 0, 0 };
        vr::TrackedDeviceIndex_t g_grip_device[2] = {
            vr::k_unTrackedDeviceIndexInvalid,
            vr::k_unTrackedDeviceIndexInvalid };
        bool g_grip_missing_reported[2] = { false, false };
        bool g_handhold_haptic_error_reported[2] = { false, false };

        bool g_ready = false;
        bool g_given_up = false;

        // What the mod itself is holding down. A key is released exactly once
        // and only if it was pressed here, so there is never a stray key-up
        // for something the player is holding on the real keyboard.
        bool g_held[256] = { false };
        bool g_mouse_left = false;

        // Stick directions, latched with hysteresis so a stick resting near
        // the threshold does not chatter the key up and down.
        bool g_stick_dir[4] = { false };  // Physical left-stick latch
        bool g_move_dir[4] = { false };   // W A S D after traversal mapping
        bool g_first_person_jump_held = false;
        bool g_nav_dir[4]  = { false };   // up left down right
        unsigned g_move_reports = 0;
        unsigned g_last_move_mask = 0;
        int g_last_ledge_pull_dir = 0;
        bool g_last_ledge_pull_up = false;
        bool g_ledge_pull_left = false;
        bool g_ledge_pull_right = false;
        bool g_ledge_pull_up = false;
        DWORD g_ledge_pull_requested_at = 0;
        bool g_ledge_climb_followthrough = false;
        // Jump is tapped, not held, for a two-hand pull-up: holding it is
        // what picks the game's slow pull-up (user 2026-10-06).
        DWORD g_ledge_jump_until = 0;
        bool g_vine_pull_up = false;
        bool g_vine_pull_down = false;
        bool g_vine_pull_left = false;
        bool g_vine_pull_right = false;
        int g_last_vine_pull_dir = 0;
        int g_last_vine_pull_side = 0;
        int g_vine_look_side = 0;
        unsigned g_vine_look_reports = 0;
        unsigned g_vine_pull_reports = 0;
        unsigned g_ledge_pull_reports = 0;
        // Hand-over-hand on a ledge or swing bar. A "stroke" is one fresh
        // grip that then pulls >= 8 cm sideways. Strokes by alternating
        // hands in the same direction, each within 1 s of the last, request
        // the retail fast traversal: FastTraversal::Update (0x00564360, run
        // by PlayerSM::Process on PlayerSM+0x4C) sets its "doing" bit while
        // the Action button (LT here, key E) is HELD, and the shimmy /
        // traverse animation scripts ask IsFastTraversing. On a bar the
        // HPoleFastTraverse flag is also set directly (camera_head.cpp).
        struct FastShimmyState
        {
            unsigned seen_serial[2] = { 0, 0 };
            unsigned stroke_serial[2] = { 0, 0 };
            DWORD regrip_at = 0;
            int last_hand = -1;
            int last_direction = 0;
            int strokes = 0;
            DWORD last_stroke_at = 0;
            int held_direction = 0;
            bool active = false;
            bool holding = false;
            DWORD released_at = 0;
            unsigned last_flags = 0xFFFFFFFFu;
            unsigned flag_reports = 0;
        };
        FastShimmyState g_fast_shimmy;
        // Hand-over-hand on a vertical hold (vine, chain, ladder): the same
        // stroke idea with the up pull. Climbing carries across the hand
        // swap, and Action (E, retail fast traversal) is held while hands
        // alternate when the game allows it there.
        FastShimmyState g_fast_climb;
        // Raw left stick this frame: x right, y forward.
        float g_move_raw[2] = { 0.0f, 0.0f };

        float g_mouse_rem[2] = { 0.0f, 0.0f };
        float g_first_person_turn = 0.0f;
        LARGE_INTEGER g_qpf{};
        LARGE_INTEGER g_last{};

        bool     g_in_menu = false;
        int      g_last_index = -2;
        short    g_last_screen = -2;
        unsigned g_frames_in_set = 0;
        bool     g_reported_bindings = false;
        bool     g_first_output = false;
        bool     g_light_grip_was = false;
        // Third-person immersive: a left grip that toggled the chest light
        // is not also the grapple key until it is released.
        bool     g_light_grip_claim = false;
        // Third-person gear cross: a d-pad of gear icons in front of the
        // waist, grabbed with either hand. Body frame metres.
        // Centre, movable with the middle handle and saved to the INI
        // ([controls] gear_cross_right/up/forward).
        float kGearCross[3] = { 0.0f, -0.42f, 0.34f };
        bool  g_gear_loaded = false;
        const float kGearSpacing = 0.10f;
        const float kGearRadius = 0.06f;      // grab reach per icon
        const float kGearNearRange = 0.35f;   // icons brighten inside this
        // The retail gear cross (user, 2026-10-01): up = medipack,
        // right = binoculars, down = weapon switch, left = flashlight.
        const WORD  kGearKeys[4] = { VK_HOME, VK_NEXT, VK_END, VK_DELETE };
        const char* const kGearNames[4] = {
            "medipack", "binoculars", "weapon switch", "flashlight" };
        // The middle handle: grip it to drag the whole cross.
        int       g_gear_drag_hand = -1;
        float     g_gear_drag_offset[3]{};
        int       g_gear_handle_hover[2] = { -1, -1 };

        void gear_cross_load()
        {
            if (g_gear_loaded)
                return;
            g_gear_loaded = true;
            wchar_t path[MAX_PATH]{}, value[32]{};
            swprintf_s(path, L"%strlvr.ini", exe_dir());
            const wchar_t* keys[3] = { L"gear_cross_right", L"gear_cross_up",
                                       L"gear_cross_forward" };
            for (int k = 0; k < 3; ++k)
            {
                GetPrivateProfileStringW(L"controls", keys[k], L"", value,
                                         32, path);
                wchar_t* end = nullptr;
                const float v = wcstof(value, &end);
                if (end != value && std::isfinite(v) && fabsf(v) < 1.5f)
                    kGearCross[k] = v;
            }
            log("controls: gear cross centre (%+.2f, %+.2f, %+.2f) m",
                kGearCross[0], kGearCross[1], kGearCross[2]);
        }

        void gear_cross_save()
        {
            wchar_t path[MAX_PATH]{}, value[32]{};
            swprintf_s(path, L"%strlvr.ini", exe_dir());
            const wchar_t* keys[3] = { L"gear_cross_right", L"gear_cross_up",
                                       L"gear_cross_forward" };
            for (int k = 0; k < 3; ++k)
            {
                swprintf_s(value, L"%.3f", kGearCross[k]);
                WritePrivateProfileStringW(L"controls", keys[k], value, path);
            }
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
            log("controls: gear cross moved and saved at (%+.2f, %+.2f, "
                "%+.2f) m", kGearCross[0], kGearCross[1], kGearCross[2]);
        }
        bool      g_gear_active = false;
        int       g_gear_hover[2] = { -1, -1 };
        bool      g_gear_grip_was[2] = { false, false };
        bool      g_gear_grip_claim[2] = { false, false };
        WORD      g_gear_pulse_key = 0;
        ULONGLONG g_gear_pulse_until = 0;
        int       g_gear_flash_item = -1;
        ULONGLONG g_gear_flash_at = 0;
        float     g_gear_nearness = 0.0f;

        void gear_item_position(int item, float out[3])
        {
            static const float dir[4][2] = {
                { 0.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, -1.0f },
                { -1.0f, 0.0f } };
            out[0] = kGearCross[0] + dir[item][0] * kGearSpacing;
            out[1] = kGearCross[1] + dir[item][1] * kGearSpacing;
            out[2] = kGearCross[2];
        }
        // Third-person immersive test log: button press edges (capped).
        bool     g_tp_button_was[32] = { false };
        unsigned g_tp_button_reports = 0;
        bool     g_tp_reported = false;
        bool     g_light_key_pulse = false;
        bool     g_reported_immersive_light = false;
        bool     g_in_movie = false;
        bool     g_holster_drawn[2] = { false, false };
        bool     g_holster_inside[2] = { false, false };
        bool     g_holster_grip_was[2] = { false, false };
        bool     g_holster_grip_consumed[2] = { false, false };
        int      g_pistol_draw_hand = -1;
        ULONGLONG g_pistol_drawn_at = 0;
        unsigned g_holster_reports[2] = { 0, 0 };
        int      g_binocular_hand = -1;
        bool     g_binocular_view = false;
        bool     g_binocular_key_pulse = false;
        int      g_grapple_hand = -1;
        // Pouches (immersive): a grenade/flare taken from the front of the
        // belt (between binoculars and grapple; first build had it on the
        // right chest) is thrown by a forward fling on release; a medipack
        // taken from the lower-left chest is used by releasing it at the
        // mouth. Body frame (metres from the HMD, yaw only): right, up,
        // forward. Both are drawn as outlines (vr_submit).
        const float kGrenadePouch[3] = { 0.0f, -0.42f, 0.06f };
        // Right chest (moved from the left; the light took that spot).
        const float kMedipackPouch[3] = { 0.13f, -0.25f, 0.03f };

        // A pouch's point: saved by the holster setup, else the default.
        void pouch_point(int item, float out[3])
        {
            if (tune_gear_position(item == 0 ? TuneGearGrenade
                                             : TuneGearMedipack, out))
                return;
            const float* p = item == 0 ? kGrenadePouch : kMedipackPouch;
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
        }
        const float kMouth[3] = { 0.0f, -0.09f, 0.07f };
        const float kPouchRadius = 0.09f;
        const float kMouthRadius = 0.13f;
        int      g_grenade_hand = -1;
        int      g_medipack_hand = -1;
        float    g_grenade_start_forward = 0.0f;
        float    g_grenade_fast_speed = 0.0f;
        ULONGLONG g_grenade_fast_at = 0;
        bool     g_grenade_throw_pulse = false;
        bool     g_medipack_pulse = false;

        bool near_point(const float* point, float right, float up,
                        float forward, float radius)
        {
            const float dx = right - point[0];
            const float dy = up - point[1];
            const float dz = forward - point[2];
            return dx * dx + dy * dy + dz * dz <= radius * radius;
        }

        void vr_input_handhold_haptic_fwd(bool left);

        // Hover and grab on the third-person gear cross. A grip press on a
        // hovered icon pulses that item's game key for 120 ms and claims
        // the grip (no grapple / lock-on key) until it is released.
        void update_gear_cross(bool active, bool left_grip, bool right_grip,
                               ULONGLONG now)
        {
            const bool grips[2] = { left_grip, right_grip };
            gear_cross_load();
            if (active != g_gear_active)
            {
                g_gear_active = active;
                log("controls: third-person gear cross %s", active
                    ? "on (up medipack, right binoculars, down weapon "
                      "switch, left flashlight; middle handle moves it)"
                    : "off");
            }
            float nearest = 1.0e9f;
            for (int hand = 0; hand < 2; ++hand)
            {
                if (!grips[hand])
                    g_gear_grip_claim[hand] = false;
                const bool pressed = grips[hand] && !g_gear_grip_was[hand];
                g_gear_grip_was[hand] = grips[hand];
                float r = 0.0f, u = 0.0f, fw = 0.0f;
                if (!active || !vr_controller_body_position(hand == 0, &r,
                                                            &u, &fw))
                {
                    g_gear_hover[hand] = -1;
                    g_gear_handle_hover[hand] = -1;
                    if (g_gear_drag_hand == hand)
                        g_gear_drag_hand = -1; // tracking lost: no save
                    continue;
                }
                // Dragging by the middle handle: the centre follows the
                // hand; releasing the grip saves it.
                if (g_gear_drag_hand == hand)
                {
                    g_gear_hover[hand] = -1;
                    if (grips[hand])
                    {
                        kGearCross[0] = r + g_gear_drag_offset[0];
                        kGearCross[1] = u + g_gear_drag_offset[1];
                        kGearCross[2] = fw + g_gear_drag_offset[2];
                    }
                    else
                    {
                        g_gear_drag_hand = -1;
                        gear_cross_save();
                    }
                    nearest = 0.0f;
                    continue;
                }
                const float cx = r - kGearCross[0];
                const float cy = u - kGearCross[1];
                const float cz = fw - kGearCross[2];
                const float to_centre = sqrtf(cx * cx + cy * cy + cz * cz);
                nearest = to_centre < nearest ? to_centre : nearest;
                int hover = -1;
                float best = kGearRadius * kGearRadius;
                for (int item = 0; item < 4; ++item)
                {
                    float p[3];
                    gear_item_position(item, p);
                    const float dx = r - p[0], dy = u - p[1], dz = fw - p[2];
                    const float d = dx * dx + dy * dy + dz * dz;
                    if (d <= best)
                    {
                        best = d;
                        hover = item;
                    }
                }
                const int handle = to_centre <= kGearRadius &&
                    (hover < 0 || to_centre * to_centre < best) ? 1 : -1;
                if (handle > 0)
                    hover = -1;
                if ((hover >= 0 && hover != g_gear_hover[hand]) ||
                    (handle > 0 && g_gear_handle_hover[hand] < 0))
                    vr_input_handhold_haptic_fwd(hand == 0);
                g_gear_hover[hand] = hover;
                g_gear_handle_hover[hand] = handle;
                if (pressed && handle > 0 && g_gear_drag_hand < 0)
                {
                    g_gear_grip_claim[hand] = true;
                    g_gear_drag_hand = hand;
                    g_gear_drag_offset[0] = kGearCross[0] - r;
                    g_gear_drag_offset[1] = kGearCross[1] - u;
                    g_gear_drag_offset[2] = kGearCross[2] - fw;
                    log("controls: [3P] %s hand took the gear cross handle",
                        hand == 0 ? "left" : "right");
                }
                else if (pressed && hover >= 0)
                {
                    g_gear_grip_claim[hand] = true;
                    g_gear_pulse_key = kGearKeys[hover];
                    g_gear_pulse_until = now + 120;
                    g_gear_flash_item = hover;
                    g_gear_flash_at = now;
                    log("controls: [3P] %s hand grabbed the %s from the gear "
                        "cross", hand == 0 ? "left" : "right",
                        kGearNames[hover]);
                }
            }
            g_gear_nearness = !active ? 0.0f
                : nearest >= kGearNearRange ? 0.0f
                : 1.0f - nearest / kGearNearRange;
            if (!active)
            {
                g_gear_pulse_until = 0;
                g_gear_pulse_key = 0;
                if (g_gear_drag_hand >= 0)
                {
                    g_gear_drag_hand = -1;
                    gear_cross_save();
                }
            }
        }
        bool     g_grapple_throw_armed = false;
        bool     g_grapple_throw_pulse = false;
        // The throw holds the grapple key 150 ms: a one-frame tap worked
        // holstered but was ignored with a pistol drawn (log 2026-10-04: no
        // hook on any armed throw; third person holds it with the grip).
        ULONGLONG g_grapple_key_until = 0;
        ULONGLONG g_grapple_key_from = 0;
        // Gear placement started from the pause menu: allowed without the
        // developer gear_tuning switch.
        bool      g_menu_tuning = false;
        ULONGLONG g_menu_request_at = 0;
        // Retail target, seen within the last moments (it flickers; log
        // 2026-10-04 had a target 120 ms after a "no target" throw).
        ULONGLONG g_grapple_target_seen_at = 0;
        // Guns put away for a no-target throw, drawn again unless the hook
        // latches onto something (user, 2026-10-04).
        bool      g_regrab_pending = false;
        bool      g_regrab_deployed = false;
        bool      g_regrab_drawn[2] = { false, false };
        int       g_regrab_kind = 0;
        int       g_regrab_hand = -1;
        ULONGLONG g_regrab_since = 0;
        float    g_grapple_start_forward = 0.0f;
        float    g_grapple_fast_speed = 0.0f;
        ULONGLONG g_grapple_fast_at = 0;
        int      g_grapple_pull_state_was = 0;
        bool     g_grapple_pull_trigger_was[2] = { false, false };
        bool     g_grapple_pull_armed[2] = { false, false };
        bool     g_grapple_pull_fired[2] = { false, false };
        float    g_grapple_pull_start_forward[2] = { 0.0f, 0.0f };
        unsigned g_item_miss_reports[2]{};
        VrControllerStyle g_controller_style = VrControllerTouch;
        bool     g_tuning_trigger_was[2] = { false, false };
        // The trigger that placed the last item is ignored until released.
        bool     g_tuning_trigger_hold[2] = { false, false };
        // Hand calibration: button edges, and whether it drew the pistols
        // (put away again when it ends).
        bool     g_cal_grip_was[2] = { false, false };
        bool     g_cal_pause_was = false;
        // Holster setup: the menu button cancels it (the right-stick
        // double-click that used to is now the PDA).
        bool     g_tuning_pause_was = false;
        // The menu button that cancelled a setup does not also pause the
        // game: held back until it is released.
        bool     g_pause_hold = false;
        // View switch: right stick long press (the binoculars action, free
        // in both immersive views) and the \ key.
        bool     g_view_button_was = false;
        // A recentre a moment after the view switch (user 2026-10-06), once
        // the new camera has settled.
        ULONGLONG g_view_recenter_at = 0;
        bool     g_cal_drew_guns = false;

        using PFN_PlayerCombat = void(__cdecl*)();
        using PFN_CombatAllowed = bool(__cdecl*)();
        const uintptr_t kPlayerInvEnterIndicatorMode = 0x005AC310;
        const uintptr_t kPlayerInvEndCombatMode = 0x005AC4B0;
        const uintptr_t kPlayerCombatAllowed = 0x005A52D0;

        bool digital(vr::VRActionHandle_t h);
        bool movie_active();

        // Weapon selection (playerInvProcess 0x005B1DA4..0x005B1DDA):
        // PlayerData+0x3E1 is the selected weapon slot, +0x3E5 the requested
        // one; when they differ and Lara can attack, retail swaps (with its
        // animation in combat). +0x3E8 + slot*8 is the slot's weapon
        // instance. playerInvSelectWeapon(slot) selects immediately.
        using PFN_SelectWeapon = void(__cdecl*)(char);
        const uintptr_t kPlayerInvSelectWeapon = 0x005A6F20;
        const uintptr_t kPlayerDataPointer = 0x0111713C;
        enum WeaponKind { WeaponNone = 0, WeaponPistols = 1, WeaponLong = 2 };

        int weapon_slot_kind(int slot)
        {
            __try
            {
                const unsigned char* pd = *reinterpret_cast<unsigned char**>(
                    kPlayerDataPointer);
                if (!pd || slot < 0 || slot > 10)
                    return WeaponNone;
                const unsigned char* inst =
                    *reinterpret_cast<unsigned char* const*>(
                        pd + 0x3E8 + slot * 8);
                if (!inst)
                    return WeaponNone;
                const unsigned char* object =
                    *reinterpret_cast<unsigned char* const*>(inst + 0x94);
                const char* name = object
                    ? *reinterpret_cast<const char* const*>(object + 0x48)
                    : nullptr;
                if (!name)
                    return WeaponNone;
                if (strncmp(name, "handgun", 7) == 0)
                    return WeaponPistols;
                const size_t n = strlen(name);
                return n > 9 && strcmp(name + n - 9, "_rbweapon") == 0
                    ? WeaponLong : WeaponNone;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return WeaponNone;
            }
        }

        int selected_weapon_slot()
        {
            __try
            {
                const unsigned char* pd = *reinterpret_cast<unsigned char**>(
                    kPlayerDataPointer);
                return pd ? static_cast<signed char>(pd[0x3E1]) : -1;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return -1;
            }
        }

        int find_weapon_slot(int kind)
        {
            const int current = selected_weapon_slot();
            if (weapon_slot_kind(current) == kind)
                return current;
            for (int slot = 0; slot <= 10; ++slot)
                if (weapon_slot_kind(slot) == kind)
                    return slot;
            return -1;
        }

        void log_weapon_slots()
        {
            static unsigned reports = 0;
            if (reports++ >= 4)
                return;
            char text[160] = {};
            int used = 0;
            for (int slot = 0; slot <= 10; ++slot)
            {
                const int kind = weapon_slot_kind(slot);
                if (kind != WeaponNone)
                    used += sprintf_s(text + used, sizeof(text) - used,
                                      " %d=%s", slot,
                                      kind == WeaponPistols ? "pistols"
                                                            : "long gun");
            }
            log("controls: weapon slots%s; selected %d", used ? text : " none",
                selected_weapon_slot());
        }

        // Make `kind` the selected weapon. Out of combat it is selected at
        // once (so the following draw brings it out); in combat the request
        // slot lets retail play its own swap. False if Lara has none.
        bool select_weapon_kind(int kind, bool in_combat)
        {
            log_weapon_slots();
            const int slot = find_weapon_slot(kind);
            if (slot < 0)
                return false;
            if (selected_weapon_slot() == slot)
                return true;
            __try
            {
                unsigned char* pd = *reinterpret_cast<unsigned char**>(
                    kPlayerDataPointer);
                if (!pd)
                    return false;
                if (in_combat)
                    pd[0x3E5] = static_cast<unsigned char>(slot);
                else
                    reinterpret_cast<PFN_SelectWeapon>(
                        kPlayerInvSelectWeapon)(static_cast<char>(slot));
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            log("controls: %s %s (slot %d)",
                in_combat ? "requested swap to" : "selected",
                kind == WeaponPistols ? "pistols" : "long gun", slot);
            return true;
        }

        // Over either shoulder: the hand behind the head's plane and no
        // lower than the chest, within reach of the body's centreline
        // (metres, HMD-yaw body frame as vr_controller_body_position).
        bool in_shoulder_zone(float right, float up, float forward)
        {
            return forward <= 0.0f && up >= -0.35f && up <= 0.25f &&
                   fabsf(right) <= 0.45f;
        }

        void reset_holster_gesture()
        {
            const bool pistols_were_drawn =
                g_holster_drawn[0] || g_holster_drawn[1];
            camera_first_person_gear_release(0);
            camera_first_person_gear_release(1);
            g_binocular_hand = -1;
            g_binocular_view = false;
            g_binocular_key_pulse = false;
            g_grapple_hand = -1;
            g_grapple_throw_armed = false;
            g_grapple_throw_pulse = false;
            g_grenade_hand = -1;
            g_medipack_hand = -1;
            g_grenade_throw_pulse = false;
            g_medipack_pulse = false;
            g_grapple_fast_at = 0;
            g_grapple_pull_state_was = 0;
            for (int hand = 0; hand < 2; ++hand)
            {
                g_grapple_pull_trigger_was[hand] = digital(
                    g_game[hand == 0 ? GameInteract : GameFire].handle);
                g_grapple_pull_armed[hand] = false;
                g_grapple_pull_fired[hand] = false;
            }
            for (int hand = 0; hand < 2; ++hand)
            {
                g_holster_drawn[hand] = false;
                g_holster_inside[hand] = false;
                g_holster_grip_consumed[hand] = false;
            }
            g_pistol_draw_hand = -1;
            g_pistol_drawn_at = 0;
            // Traversal, UI entry, or focus loss can end the gesture before
            // a hip return. Keep the game's combat mode in step with the
            // mod's gun pose while first-person gameplay is still active.
            if (pistols_were_drawn && camera_first_person_active() &&
                !movie_active())
            {
                reinterpret_cast<PFN_PlayerCombat>(
                    kPlayerInvEndCombatMode)();
                log("controls: ended pistol combat when immersive input "
                    "context changed");
            }
        }

        // suspended: a menu is open. Pausing used to reset the gesture,
        // which cleared the VR draw state and called EndCombatMode while
        // the game was paused; if retail did not act on it, Lara came back
        // with pistols out that the mod thought were holstered (open hands,
        // retail-placed guns). Now a pause leaves everything as it was.
        void update_holster_gesture(bool active, bool suspended = false)
        {
            if (!active && suspended)
            {
                g_holster_grip_was[0] = digital(g_game[GameGrapple].handle);
                g_holster_grip_was[1] = digital(g_game[GameLockOn].handle);
                return;
            }
            if (!active)
            {
                reset_holster_gesture();
                g_holster_grip_was[0] = digital(g_game[GameGrapple].handle);
                g_holster_grip_was[1] = digital(g_game[GameLockOn].handle);
                return;
            }
            bool toggled_this_frame = false;
            const ULONGLONG now = GetTickCount64();
            for (int hand = 0; hand < 2; ++hand)
            {
                const VrHolsterZone& zone = tune_holster_zone();
                const bool left = hand == 0;
                const bool grip = digital(g_game[left ? GameGrapple :
                    GameLockOn].handle);
                const bool grip_pressed = grip && !g_holster_grip_was[hand];
                const bool grip_released = !grip && g_holster_grip_was[hand];
                g_holster_grip_was[hand] = grip;
                if (!grip)
                    g_holster_grip_consumed[hand] = false;
                // The free hand at the long gun's fore-end: its grip holds
                // the gun, not the holster, belt items or grapple.
                if (camera_two_hand_grip_claims(left))
                {
                    if (grip)
                        g_holster_grip_consumed[hand] = true;
                    continue;
                }
                float right = 0.0f, up = 0.0f, forward = 0.0f;
                if (!vr_controller_body_position(
                        left, &right, &up, &forward))
                {
                    g_holster_inside[hand] = false;
                    if (hand == g_grapple_hand && grip_released)
                    {
                        g_grapple_throw_armed = false;
                        log("controls: grapple retained; controller tracking "
                            "unavailable at release");
                    }
                    continue;
                }

                // Chest pouches: an item in hand first, then a new pickup.
                if (hand == g_grenade_hand)
                {
                    g_holster_grip_consumed[hand] = true;
                    float speed = 0.0f;
                    if (grip && vr_controller_body_forward_speed(left, &speed) &&
                        speed >= 1.15f)
                    {
                        g_grenade_fast_speed = speed;
                        g_grenade_fast_at = now;
                    }
                    if (grip_released)
                    {
                        const float recent = g_grenade_fast_at &&
                            now - g_grenade_fast_at <= 180
                            ? g_grenade_fast_speed : 0.0f;
                        const float travel =
                            forward - g_grenade_start_forward;
                        g_grenade_throw_pulse = vr_gesture_grapple_throw(
                            travel, forward, recent);
                        g_grenade_hand = -1;
                        if (g_grenade_throw_pulse)
                            vr_input_weapon_haptic(left, 0.6f);
                        log("controls: grenade/flare %s (forward %.2f m, "
                            "travel %.2f m, recent speed %.2f m/s)",
                            g_grenade_throw_pulse ? "thrown"
                                                  : "put back",
                            forward, travel, recent);
                    }
                    continue;
                }
                if (hand == g_medipack_hand)
                {
                    g_holster_grip_consumed[hand] = true;
                    if (grip_released)
                    {
                        const bool at_mouth = near_point(kMouth, right, up,
                            forward, kMouthRadius);
                        g_medipack_hand = -1;
                        g_medipack_pulse = at_mouth;
                        if (at_mouth)
                            vr_input_weapon_haptic(left, 0.4f);
                        log("controls: medipack %s (hand at right %+.2f, up "
                            "%+.2f, forward %+.2f m)", at_mouth
                            ? "used at the mouth" : "put back", right, up,
                            forward);
                    }
                    continue;
                }
                if (grip_pressed && hand != g_binocular_hand &&
                    hand != g_grapple_hand)
                {
                    float grenade_pouch[3], medipack_pouch[3];
                    pouch_point(0, grenade_pouch);
                    pouch_point(1, medipack_pouch);
                    if (near_point(grenade_pouch, right, up, forward,
                                   kPouchRadius))
                    {
                        g_grenade_hand = hand;
                        g_grenade_start_forward = forward;
                        g_grenade_fast_at = 0;
                        g_grenade_fast_speed = 0.0f;
                        g_holster_grip_consumed[hand] = true;
                        vr_input_handhold_haptic(left);
                        log("controls: %s hand took a grenade/flare from the "
                            "chest pouch", left ? "left" : "right");
                        continue;
                    }
                    if (near_point(medipack_pouch, right, up, forward,
                                   kPouchRadius))
                    {
                        g_medipack_hand = hand;
                        g_holster_grip_consumed[hand] = true;
                        vr_input_handhold_haptic(left);
                        log("controls: %s hand took a medipack from the "
                            "chest pouch", left ? "left" : "right");
                        continue;
                    }
                    if (up > -0.40f && up < -0.05f && forward > -0.05f &&
                        fabsf(right) < 0.30f)
                    {
                        static unsigned near_reports = 0;
                        if (near_reports++ < 8)
                            log("controls: %s grip at the chest outside the "
                                "pouches (right %+.2f, up %+.2f, forward "
                                "%+.2f m)", left ? "left" : "right", right,
                                up, forward);
                    }
                }

                if (hand == g_binocular_hand)
                {
                    const bool at_face = vr_gesture_binoculars_at_face(
                        right, up, forward);
                    float belt_distance = 0.0f;
                    const bool at_belt = grip_pressed &&
                        camera_first_person_gear_belt_distance(
                            0, left, &belt_distance) &&
                        belt_distance <= 0.14f;
                    if (at_belt ||
                        (grip_pressed && at_face && g_binocular_view))
                    {
                        const bool was_viewing = g_binocular_view;
                        if (was_viewing)
                        {
                            g_binocular_view = false;
                            g_binocular_key_pulse = true;
                        }
                        g_binocular_hand = -1;
                        camera_first_person_gear_release(0);
                        g_holster_grip_consumed[hand] = true;
                        log("controls: %s grip %s returned binoculars "
                            "to belt%s", left ? "left" : "right",
                            at_belt ? "at belt" : "at face",
                            was_viewing ? " and closed view" : "");
                    }
                    else
                    {
                        if (at_face && !g_binocular_view)
                        {
                            g_binocular_view = true;
                            g_binocular_key_pulse = true;
                            log("controls: binoculars reached face; opened view");
                        }
                        if (grip)
                            g_holster_grip_consumed[hand] = true;
                    }
                    continue;
                }

                if (hand == g_grapple_hand)
                {
                    float belt_distance = 0.0f;
                    if (grip_pressed &&
                        camera_first_person_gear_belt_distance(
                            1, left, &belt_distance) &&
                        belt_distance <= 0.14f)
                    {
                        g_grapple_hand = -1;
                        g_grapple_throw_armed = false;
                        camera_first_person_gear_release(1);
                        g_holster_grip_consumed[hand] = true;
                        log("controls: %s grip at belt returned grapple "
                            "without throwing", left ? "left" : "right");
                        continue;
                    }
                    if (grip_pressed && !g_grapple_throw_armed)
                    {
                        g_grapple_throw_armed = true;
                        g_grapple_start_forward = forward;
                        g_grapple_fast_at = 0;
                        g_grapple_fast_speed = 0.0f;
                    }
                    float speed = 0.0f;
                    if (grip && g_grapple_throw_armed &&
                        vr_controller_body_forward_speed(left, &speed) &&
                        speed >= 1.15f)
                    {
                        g_grapple_fast_speed = speed;
                        g_grapple_fast_at = now;
                    }
                    if (grip_released && g_grapple_throw_armed)
                    {
                        const float recent_speed = g_grapple_fast_at &&
                            now - g_grapple_fast_at <= 180
                            ? g_grapple_fast_speed : 0.0f;
                        const float travel =
                            forward - g_grapple_start_forward;
                        g_grapple_throw_pulse = vr_gesture_grapple_throw(
                            travel, forward, recent_speed);
                        g_grapple_throw_armed = false;
                        if (g_grapple_throw_pulse)
                        {
                            g_grapple_hand = -1;
                            camera_first_person_gear_release(1);
                        }
                        log("controls: grapple %s on release "
                            "(forward %.2f m, travel %.2f m, "
                            "recent speed %.2f m/s)",
                            g_grapple_throw_pulse ? "thrown" : "retained",
                            forward, travel, recent_speed);
                    }
                    else if (grip)
                        g_holster_grip_consumed[hand] = true;
                    continue;
                }

                // Belt items. The binoculars need both hands free; the
                // grapple can be taken by any hand but the one holding the
                // long gun -- with one pistol out, the long gun, or a pistol
                // in that very hand (user, 2026-10-04: in combat the grapple
                // could not be reached at all).
                const bool any_gun = g_holster_drawn[0] || g_holster_drawn[1];
                const bool long_gun_hand = any_gun &&
                    weapon_slot_kind(selected_weapon_slot()) == WeaponLong &&
                    hand == (config().left_handed ? 0 : 1);
                if (grip_pressed && !long_gun_hand)
                {
                    int item = -1;
                    float nearest_distance = 1.0f;
                    for (int candidate = 0; candidate < 2; ++candidate)
                    {
                        if ((candidate == 0 &&
                             (g_binocular_hand >= 0 || any_gun)) ||
                            (candidate == 1 && g_grapple_hand >= 0))
                            continue;
                        float distance = 0.0f;
                        if (camera_first_person_gear_belt_distance(
                                candidate, left, &distance) &&
                            distance < nearest_distance)
                        {
                            item = candidate;
                            nearest_distance = distance;
                        }
                    }
                    float distance = nearest_distance;
                    if (item >= 0 && camera_first_person_gear_grab(
                            item, left, 0.14f, &distance))
                    {
                        g_holster_grip_consumed[hand] = true;
                        if (item == 0)
                        {
                            g_binocular_hand = hand;
                            log("controls: %s grip picked up binoculars "
                                "(%.2f m from visible item)",
                                left ? "left" : "right", distance);
                        }
                        else
                        {
                            g_grapple_hand = hand;
                            g_grapple_throw_armed = true;
                            g_grapple_start_forward = forward;
                            g_grapple_fast_at = 0;
                            g_grapple_fast_speed = 0.0f;
                            log("controls: %s grip picked up grapple "
                                "(%.2f m from visible item)",
                                left ? "left" : "right", distance);
                        }
                        continue;
                    }
                    if (item >= 0 && distance < 0.45f &&
                        g_item_miss_reports[item]++ < 12)
                        log("controls: %s grip %.2f m from %s; "
                            "pickup needs 0.14 m",
                            left ? "left" : "right", distance,
                            item == 0 ? "binoculars" : "grapple");
                }
                // Shoulder: the long gun. Draw it (selecting it first), swap
                // to it from the pistols, or put it away if already out.
                if (grip_pressed && !toggled_this_frame &&
                    in_shoulder_zone(right, up, forward))
                {
                    g_holster_grip_consumed[hand] = true;
                    toggled_this_frame = true;
                    const bool drawn = g_holster_drawn[0] ||
                                       g_holster_drawn[1];
                    log("controls: %s shoulder reach (right %+.2f, up %+.2f, "
                        "forward %+.2f metres)", left ? "left" : "right",
                        right, up, forward);
                    if (find_weapon_slot(WeaponLong) < 0)
                    {
                        log_weapon_slots();
                        log("controls: shoulder grip -- Lara has no long gun");
                        continue;
                    }
                    const bool long_out = weapon_slot_kind(
                        selected_weapon_slot()) == WeaponLong;
                    if (drawn && long_out)
                    {
                        g_holster_drawn[0] = g_holster_drawn[1] = false;
                        g_pistol_draw_hand = -1;
                        g_pistol_drawn_at = 0;
                        log("controls: %s shoulder grip put the long gun away",
                            left ? "left" : "right");
                        reinterpret_cast<PFN_PlayerCombat>(
                            kPlayerInvEndCombatMode)();
                    }
                    else if (drawn)
                    {
                        select_weapon_kind(WeaponLong, true);
                        g_holster_drawn[0] = g_holster_drawn[1] = true;
                    }
                    else
                    {
                        bool allowed = false;
                        __try
                        {
                            allowed = reinterpret_cast<PFN_CombatAllowed>(
                                kPlayerCombatAllowed)();
                        }
                        __except(EXCEPTION_EXECUTE_HANDLER)
                        {
                            allowed = false;
                        }
                        if (!allowed)
                        {
                            log("controls: %s shoulder grip; combat denied",
                                left ? "left" : "right");
                            continue;
                        }
                        select_weapon_kind(WeaponLong, false);
                        g_holster_drawn[0] = g_holster_drawn[1] = true;
                        g_pistol_draw_hand = hand;
                        g_pistol_drawn_at = now;
                        reinterpret_cast<PFN_PlayerCombat>(
                            kPlayerInvEnterIndicatorMode)();
                        log("controls: %s shoulder grip drew the long gun",
                            left ? "left" : "right");
                    }
                    continue;
                }
                // Near-misses, to tune the shoulder zone from the log.
                if (grip_pressed && up >= -0.35f && forward < 0.12f &&
                    fabsf(right) <= 0.5f)
                {
                    static unsigned near_reports = 0;
                    if (near_reports++ < 8)
                        log("controls: %s grip near the shoulder but outside "
                            "it (right %+.2f, up %+.2f, forward %+.2f)",
                            left ? "left" : "right", right, up, forward);
                }

                const bool inside = vr_gesture_in_holster_zone(
                    left, right, up, forward, &zone);
                const bool entered = inside && !g_holster_inside[hand];
                g_holster_inside[hand] = inside;
                if (entered &&
                    g_holster_reports[hand]++ < 8)
                    log("controls: %s hip reach (right %+.2f, up %+.2f, "
                        "forward %+.2f metres)",
                        left ? "left" : "right", right, up, forward);
                if (!inside || !grip_pressed)
                    continue;
                g_holster_grip_consumed[hand] = true;
                if (toggled_this_frame)
                    continue;
                toggled_this_frame = true;
                // Hip with the long gun out: swap to the pistols instead of
                // putting everything away.
                if ((g_holster_drawn[0] || g_holster_drawn[1]) &&
                    weapon_slot_kind(selected_weapon_slot()) == WeaponLong &&
                    find_weapon_slot(WeaponPistols) >= 0)
                {
                    select_weapon_kind(WeaponPistols, true);
                    if (config().single_pistols)
                    {
                        // Only this hip's pistol comes out.
                        g_holster_drawn[0] = hand == 0;
                        g_holster_drawn[1] = hand == 1;
                    }
                    log("controls: %s hip grip swapped the long gun for the "
                        "pistol%s", left ? "left" : "right",
                        config().single_pistols ? "" : "s");
                    continue;
                }
                // Single-pistol mode: this hip holds this hand's pistol only.
                if (config().single_pistols &&
                    (g_holster_drawn[0] || g_holster_drawn[1]))
                {
                    const int other = 1 - hand;
                    if (g_holster_drawn[hand])
                    {
                        g_holster_drawn[hand] = false;
                        if (!g_holster_drawn[other])
                        {
                            g_pistol_draw_hand = -1;
                            g_pistol_drawn_at = 0;
                            reinterpret_cast<PFN_PlayerCombat>(
                                kPlayerInvEndCombatMode)();
                        }
                        log("controls: %s hip grip holstered the %s pistol%s",
                            left ? "left" : "right", left ? "left" : "right",
                            g_holster_drawn[other] ? "" : "; combat ended");
                    }
                    else
                    {
                        g_holster_drawn[hand] = true;
                        log("controls: %s hip grip drew the %s pistol "
                            "(akimbo)", left ? "left" : "right",
                            left ? "left" : "right");
                    }
                    continue;
                }
                if (g_holster_drawn[0] || g_holster_drawn[1])
                {
                    if (hand != g_pistol_draw_hand &&
                        now - g_pistol_drawn_at < 1000)
                    {
                        log("controls: %s hip grip ignored during "
                            "other-hand draw cooldown",
                            left ? "left" : "right");
                        continue;
                    }
                    g_holster_drawn[0] = g_holster_drawn[1] = false;
                    g_pistol_draw_hand = -1;
                    g_pistol_drawn_at = 0;
                    log("controls: %s grip holstered both pistols",
                        left ? "left" : "right");
                    reinterpret_cast<PFN_PlayerCombat>(
                        kPlayerInvEndCombatMode)();
                }
                else
                {
                    bool allowed = false;
                    __try
                    {
                        allowed = reinterpret_cast<PFN_CombatAllowed>(
                            kPlayerCombatAllowed)();
                    }
                    __except(EXCEPTION_EXECUTE_HANDLER)
                    {
                        allowed = false;
                    }
                    if (!allowed)
                    {
                        log("controls: %s grip in holster zone; combat denied",
                            left ? "left" : "right");
                        continue;
                    }
                    // The hip holds the pistols: select them first so a
                    // selected long gun does not come out here.
                    select_weapon_kind(WeaponPistols, false);
                    if (config().single_pistols)
                    {
                        g_holster_drawn[0] = hand == 0;
                        g_holster_drawn[1] = hand == 1;
                    }
                    else
                        g_holster_drawn[0] = g_holster_drawn[1] = true;
                    g_pistol_draw_hand = hand;
                    g_pistol_drawn_at = now;
                    reinterpret_cast<PFN_PlayerCombat>(
                        kPlayerInvEnterIndicatorMode)();
                    log("controls: %s grip drew %s; "
                        "combat remains until grip-holstered",
                        left ? "left" : "right",
                        config().single_pistols
                            ? (left ? "the left pistol" : "the right pistol")
                            : "both pistols");
                }
            }
        }

        int update_grapple_pull(bool active, bool left_trigger,
                                bool right_trigger, bool* pulse)
        {
            const int state = active &&
                !g_holster_drawn[0] && !g_holster_drawn[1]
                ? camera_first_person_grapple_state() : 0;
            if (state != g_grapple_pull_state_was)
            {
                g_grapple_pull_state_was = state;
                log("controls: grapple hook %s",
                    state == 2 ? "attached; hold either trigger and pull back"
                    : state == 1 ? "deployed; hold either trigger and pull back"
                                 : "stowed or released");
            }
            const bool triggers[2] = { left_trigger, right_trigger };
            for (int hand = 0; hand < 2; ++hand)
            {
                const bool down = triggers[hand] &&
                    !g_tuning_trigger_hold[hand];
                const bool pressed = down &&
                    !g_grapple_pull_trigger_was[hand];
                g_grapple_pull_trigger_was[hand] = down;
                // The target query can lag the live hook handle. A yank is
                // still gesture-gated while deployed; retail ignores E until
                // a target actually supports the pull action.
                if (state == 0 || !down)
                {
                    g_grapple_pull_armed[hand] = false;
                    if (!down)
                        g_grapple_pull_fired[hand] = false;
                    continue;
                }
                float right = 0.0f, up = 0.0f, forward = 0.0f;
                if (!vr_controller_body_position(hand == 0, &right, &up,
                                                 &forward))
                {
                    g_grapple_pull_armed[hand] = false;
                    continue;
                }
                if (pressed)
                {
                    g_grapple_pull_start_forward[hand] = forward;
                    g_grapple_pull_armed[hand] = true;
                }
                if (!g_grapple_pull_armed[hand] ||
                    g_grapple_pull_fired[hand])
                    continue;
                float speed = 0.0f;
                if (!vr_controller_body_forward_speed(hand == 0, &speed))
                    speed = 0.0f;
                const float travel =
                    g_grapple_pull_start_forward[hand] - forward;
                if (vr_gesture_grapple_pull(travel, -speed))
                {
                    *pulse = true;
                    g_grapple_pull_fired[hand] = true;
                    log("controls: %s trigger grapple yank sent E "
                        "(back %.2f m, speed %.2f m/s)",
                        hand == 0 ? "left" : "right", travel, -speed);
                }
            }
            return state;
        }

        bool movie_active()
        {
            // Retail MOVIE_StartPlay stores its Bink handle at 0x0100257C;
            // MOVIE_StopPlay clears it. The UI screen stack is -1 for parts
            // of startup playback, so it cannot classify this input context.
            __try
            {
                return *reinterpret_cast<void* const*>(0x0100257C) != nullptr;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool extended(WORD vk)
        {
            switch (vk)
            {
            case VK_HOME: case VK_END: case VK_DELETE: case VK_INSERT:
            case VK_PRIOR: case VK_NEXT:
            case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
                return true;
            default:
                return false;
            }
        }

        void send_key(WORD vk, bool down)
        {
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            in.ki.dwFlags = KEYEVENTF_SCANCODE
                          | (extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0)
                          | (down ? 0 : KEYEVENTF_KEYUP);
            SendInput(1, &in, sizeof(in));
        }

        void send_mouse(DWORD flags, LONG dx, LONG dy)
        {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dx = dx;
            in.mi.dy = dy;
            in.mi.dwFlags = flags;
            SendInput(1, &in, sizeof(in));
        }

        void note_output(const char* what)
        {
            if (g_first_output)
                return;
            g_first_output = true;
            log("controls: first input sent to the game (%s)", what);
        }

        // Bring what the mod is holding into line with what is wanted now.
        void apply(const bool want[256], bool want_mouse_left)
        {
            for (int vk = 1; vk < 256; ++vk)
            {
                if (want[vk] == g_held[vk])
                    continue;
                send_key((WORD)vk, want[vk]);
                g_held[vk] = want[vk];
                if (want[vk])
                    note_output("a key");
            }
            if (want_mouse_left != g_mouse_left)
            {
                if (want_mouse_left)
                    window_keep_cursor_inside();
                send_mouse(want_mouse_left ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP,
                           0, 0);
                g_mouse_left = want_mouse_left;
                if (want_mouse_left)
                    note_output("the left mouse button");
            }
        }

        // keep_gear: leave drawn guns and held gear alone (hand calibration
        // releases every key each frame but wants the pistols out; resetting
        // the holster gesture ended combat at once, user test 2026-10-06).
        void release_all(bool keep_gear = false)
        {
            static const bool none[256] = { false };
            apply(none, false);
            for (bool& d : g_stick_dir) d = false;
            for (bool& d : g_move_dir) d = false;
            g_first_person_jump_held = false;
            g_last_ledge_pull_dir = 0;
            g_last_ledge_pull_up = false;
            g_ledge_pull_left = false;
            g_ledge_pull_right = false;
            g_ledge_pull_up = false;
            g_ledge_pull_requested_at = 0;
            g_ledge_climb_followthrough = false;
            g_fast_shimmy = FastShimmyState{};
            g_vine_pull_up = false;
            g_vine_pull_down = false;
            g_vine_pull_left = false;
            g_vine_pull_right = false;
            g_last_vine_pull_dir = 0;
            g_last_vine_pull_side = 0;
            g_vine_look_side = 0;
            for (bool& d : g_nav_dir)  d = false;
            g_mouse_rem[0] = g_mouse_rem[1] = 0.0f;
            g_light_grip_was = false;
            g_light_key_pulse = false;
            if (!keep_gear)
                reset_holster_gesture();
            camera_first_person_ledge_grip_input(
                true, digital(g_game[GameGrapple].handle), false);
            camera_first_person_ledge_grip_input(
                false, digital(g_game[GameLockOn].handle), false);
        }

        unsigned fast_traversal_flags()
        {
            __try
            {
                const unsigned char* sm =
                    *reinterpret_cast<unsigned char* const*>(0x01117554);
                return sm ? *reinterpret_cast<const unsigned*>(sm + 0x50) : 0;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        void fast_traversal_report(FastShimmyState& f, const char* where)
        {
            const unsigned flags = fast_traversal_flags() & 3u;
            if (flags != f.last_flags && f.flag_reports < 40)
            {
                ++f.flag_reports;
                log("controls: %s fast traversal: retail %s (flags 0x%X)",
                    where, (flags & 2u) ? "fast-traversing"
                         : (flags & 1u) ? "allowed, not yet fast"
                                        : "not allowed now", flags);
            }
            f.last_flags = flags;
        }

        // Vertical hand-over-hand: a stroke is a fresh grip that pulls the
        // body >= 8 cm up or down. Returns the vertical direction to send
        // (+1 up, -1 down, 0 none), carrying it across a hand swap.
        int climb_hand_over_hand(const HandholdPullSample hands[2],
                                 int direction)
        {
            const DWORD now = GetTickCount();
            FastShimmyState& f = g_fast_climb;
            bool holding = false;
            for (int hand = 0; hand < 2; ++hand)
            {
                const HandholdPullSample& sample = hands[hand];
                if (!sample.grip_serial)
                    continue;
                holding = true;
                if (sample.grip_serial == f.stroke_serial[hand] ||
                    fabsf(sample.up_metres) < 0.08f)
                    continue;
                f.stroke_serial[hand] = sample.grip_serial;
                const int stroke = sample.up_metres > 0.0f ? 1 : -1;
                if (f.last_hand == 1 - hand && f.last_direction == stroke &&
                    now - f.last_stroke_at <= 1600)
                    ++f.strokes;
                else
                    f.strokes = 1;
                f.last_hand = hand;
                f.last_direction = stroke;
                f.last_stroke_at = now;
            }
            if (f.holding && !holding)
                f.released_at = now;
            f.holding = holding;
            if (direction == 0 && f.last_direction != 0)
            {
                // Between strokes of alternating hands, or across one
                // release-and-regrab.
                if ((f.strokes >= 2 && now - f.last_stroke_at <= 1100) ||
                    (!holding && now - f.released_at <= 450 &&
                     now - f.last_stroke_at <= 1600))
                    direction = f.last_direction;
            }
            const bool active = f.strokes >= 2 &&
                now - f.last_stroke_at <= 1600 &&
                direction != 0 && direction == f.last_direction;
            if (active != f.active)
                log("controls: vertical hand-over-hand %s (%d alternating "
                    "strokes)", active ? "-> continuous climb" : "ended",
                    f.strokes);
            f.active = active;
            if (active)
                fast_traversal_report(f, "vertical");
            else
                f.last_flags = 0xFFFFFFFFu;
            return direction;
        }

        // Updates the stroke detector and returns the lateral direction to
        // send. Swapping hands briefly drops the averaged pull to zero (the
        // new hand starts at its anchor while the old one lets go), which
        // stopped the traverse on every swap; hold the previous direction
        // through that gap.
        int hand_over_hand(const HandholdPullSample hands[2],
                           int direction, bool bar)
        {
            const DWORD now = GetTickCount();
            FastShimmyState& f = g_fast_shimmy;
            for (int hand = 0; hand < 2; ++hand)
            {
                const HandholdPullSample& sample = hands[hand];
                if (!sample.grip_serial)
                    continue;
                if (sample.grip_serial != f.seen_serial[hand])
                {
                    f.seen_serial[hand] = sample.grip_serial;
                    f.regrip_at = now;
                }
                // 6 cm and 1.4 s (2026-10-05, were 8 cm and 1 s): the
                // user found fast shimmy hard to keep going.
                if (sample.grip_serial == f.stroke_serial[hand] ||
                    fabsf(sample.right_metres) < 0.06f)
                    continue;
                f.stroke_serial[hand] = sample.grip_serial;
                const int stroke = sample.right_metres > 0.0f ? 1 : -1;
                if (f.last_hand == 1 - hand && f.last_direction == stroke &&
                    now - f.last_stroke_at <= 1400)
                    ++f.strokes;
                else
                    f.strokes = 1;
                f.last_hand = hand;
                f.last_direction = stroke;
                f.last_stroke_at = now;
            }

            // One grip pulls one way. Once it has made a stroke, an opposite
            // reading from it means Lara's body overtook the world anchor
            // (fast traverse carries her further than the hand moved), not
            // a reversal: in the first test that flipped her to the other
            // direction and cancelled fast traverse after one stroke. A
            // real reversal starts with a fresh grip, which registers its
            // stroke (8 cm) before the 10 cm key latch.
            const bool overtaken = direction != 0 &&
                direction == -f.last_direction &&
                now - f.last_stroke_at <= 1500;
            if (overtaken)
                direction = 0;
            bool holding = false;
            for (int hand = 0; hand < 2; ++hand)
                holding = holding || hands[hand].grip_serial != 0;
            if (f.holding && !holding)
                f.released_at = now;
            f.holding = holding;
            if (direction == 0 && f.held_direction != 0 &&
                now - f.regrip_at <= 400)
                direction = f.held_direction;
            // While hands keep alternating, carry the direction between
            // strokes so the retail fast-traverse chain is not interrupted.
            // 2026-10-05: also with no hand held (released before the
            // other hand grabbed), 1 s (was 700 ms, holding only).
            if (direction == 0 && f.strokes >= 2 &&
                now - f.last_stroke_at <= 1000)
                direction = f.last_direction;
            // A single stroke: bridge the gap between letting go and the
            // next grab (log: the shimmy stopped at every hand swap).
            if (direction == 0 && !holding && f.last_direction != 0 &&
                now - f.released_at <= 450 &&
                now - f.last_stroke_at <= 1500)
                direction = f.last_direction;
            f.held_direction = direction;

            const bool active = f.strokes >= 2 &&
                now - f.last_stroke_at <= 1400 &&
                direction != 0 && direction == f.last_direction;
            if (active != f.active)
                log("controls: %s hand-over-hand %s (%d alternating "
                    "strokes)", bar ? "bar" : "ledge",
                    active ? "-> fast traverse" : "ended", f.strokes);
            f.active = active;

            // Proof from the game: FastTraversal's flags at PlayerSM+0x50;
            // bit 1 = allowed here, bit 2 = currently fast-traversing.
            // Logged on every change while active (the one-shot report
            // caught transitional frames: "not allowed" half the time).
            if (active)
                fast_traversal_report(f, bar ? "bar" : "ledge");
            else
                f.last_flags = 0xFFFFFFFFu;
            return direction;
        }

        bool left_hand_at_chest()
        {
            float right = 0.0f;
            float up = 0.0f;
            float forward = 0.0f;
            return vr_controller_body_position(true, &right, &up, &forward) &&
                   vr_gesture_in_chest_zone(right, up, forward,
                                            tune_light_zone_centre());
        }

        // Only ever type into the game. SendInput goes to whatever has focus,
        // and a controller left on a desk while you alt-tab should not be
        // pressing keys in your browser.
        bool game_has_focus()
        {
            HWND fg = GetForegroundWindow();
            if (!fg)
                return false;
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            return pid == GetCurrentProcessId();
        }

        // The engine's UI screen stack -- see the note at the top.
        int screen_index()
        {
            return *(volatile const int*)0x00F16E90;
        }

        // GetTopScreenID: movsx eax, word ptr [[0x01116158 + index*4]]. The
        // engine makes exactly this read whenever the index is non-negative,
        // so the pointer is valid when it does.
        short top_screen_id(int index)
        {
            if (index < 0 || index > 63)
                return -1;
            const short* const* stack = (const short* const*)0x01116158;
            const short* entry = stack[index];
            return entry ? *entry : (short)-1;
        }

        // Pressed past `on`, released below `off`.
        bool latch(bool was, float v, float on, float off)
        {
            return was ? (v > off) : (v > on);
        }

        // A dead zone, then a square: fine control near the centre, fast at
        // the edge -- roughly the shape a mouse user gets from their wrist.
        float shape(float v)
        {
            const float dz = 0.12f;
            const float a = fabsf(v);
            if (a <= dz)
                return 0.0f;
            const float s = (a - dz) / (1.0f - dz);
            return (v < 0.0f ? -1.0f : 1.0f) * s * s;
        }

        bool digital(vr::VRActionHandle_t h)
        {
            if (!h)
                return false;
            vr::InputDigitalActionData_t d{};
            return g_input->GetDigitalActionData(h, &d, sizeof(d),
                                                 vr::k_ulInvalidInputValueHandle)
                       == vr::VRInputError_None
                && d.bActive && d.bState;
        }

        void analog(vr::VRActionHandle_t h, float* x, float* y)
        {
            *x = *y = 0.0f;
            if (!h)
                return;
            vr::InputAnalogActionData_t a{};
            if (g_input->GetAnalogActionData(h, &a, sizeof(a),
                                             vr::k_ulInvalidInputValueHandle)
                    != vr::VRInputError_None || !a.bActive)
                return;
            *x = a.x;
            *y = a.y;
        }

        // Which bindings file SteamVR reaches for is decided by the controller
        // type, and guessing it wrong fails silently -- the G2's controllers
        // are "hpmotioncontroller", not the older "holographic_controller",
        // and nothing reports the mismatch. So say.
        void log_controllers()
        {
            if (!g_system)
                return;
            int found = 0;
            for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i)
            {
                if (g_system->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller)
                    continue;
                ++found;
                char type[128]{};
                vr::ETrackedPropertyError err = vr::TrackedProp_Success;
                g_system->GetStringTrackedDeviceProperty(
                    i, vr::Prop_ControllerType_String, type, sizeof(type), &err);
                const vr::ETrackedControllerRole role =
                    g_system->GetControllerRoleForTrackedDeviceIndex(i);
                if (_stricmp(type, "knuckles") == 0)
                    g_controller_style = VrControllerIndex;
                else if (_stricmp(type, "holographic_controller") == 0)
                    g_controller_style = VrControllerWmr;
                const char* hand = role == vr::TrackedControllerRole_LeftHand  ? "left"
                                 : role == vr::TrackedControllerRole_RightHand ? "right"
                                 : "unassigned";
                log("controls: %s hand is a \"%s\" -- SteamVR will use "
                    "bindings_%s.json", hand, type[0] ? type : "?",
                    type[0] ? type : "?");
            }
            if (!found)
                log("controls: no controllers switched on yet -- they are "
                    "picked up whenever they appear");
        }

        // An action with a handle but no binding reports inactive for ever,
        // which looks exactly like a controller that is switched off. Count
        // them once per set, a moment after the set first goes live.
        void report_bound()
        {
            int bound = 0;
            for (int i = 0; i < kGameButtons; ++i)
            {
                vr::InputDigitalActionData_t d{};
                if (g_game[i].handle &&
                    g_input->GetDigitalActionData(g_game[i].handle, &d, sizeof(d),
                                                  vr::k_ulInvalidInputValueHandle)
                        == vr::VRInputError_None && d.bActive)
                    ++bound;
            }
            log("controls: %d of %d controller actions are bound", bound,
                kGameButtons);
            if (bound == 0)
                log("controls:   nothing is bound -- SteamVR did not match a "
                    "bindings file to these controllers. The lines above say "
                    "which type it is looking for; SteamVR > Settings > "
                    "Controllers > Manage Controller Bindings fixes it by hand.");
        }

        bool connect()
        {
            if (g_ready)
                return true;
            if (g_given_up || !vr_ready())
                return false;

            g_input  = (vr::IVRInput*)vr_get_interface(vr::IVRInput_Version);
            g_system = (vr::IVRSystem*)vr_get_interface(vr::IVRSystem_Version);
            if (!g_input)
            {
                log("controls: no %s -- the controllers will not reach the game",
                    vr::IVRInput_Version);
                g_given_up = true;
                return false;
            }

            wchar_t wide[MAX_PATH]{};
            swprintf_s(wide, L"%strlvr_actions.json", exe_dir());
            char path[MAX_PATH * 3]{};
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, (int)sizeof(path),
                                nullptr, nullptr);

            const vr::EVRInputError merr = g_input->SetActionManifestPath(path);
            if (merr != vr::VRInputError_None)
            {
                log("controls: SetActionManifestPath(%s) failed, EVRInputError %d",
                    path, (int)merr);
                g_given_up = true;
                return false;
            }

            if (g_input->GetActionSetHandle("/actions/gameplay", &g_set_game)
                    != vr::VRInputError_None)
            {
                log("controls: the manifest has no gameplay action set");
                g_given_up = true;
                return false;
            }

            struct Named { const char* path; vr::VRActionHandle_t* h; };
            const Named sticks[] = {
                { "/actions/gameplay/in/move", &g_move },
                { "/actions/gameplay/in/look", &g_look },
                { "/actions/gameplay/in/flick", &g_flick_action },
            };
            for (const Named& s : sticks)
                if (g_input->GetActionHandle(s.path, s.h) != vr::VRInputError_None || !*s.h)
                    log("controls: action %s did not resolve", s.path);
            for (Button& b : g_game)
                if (g_input->GetActionHandle(b.path, &b.handle) != vr::VRInputError_None || !b.handle)
                    log("controls: action %s did not resolve", b.path);
            const char* haptic_paths[2] = {
                "/actions/gameplay/out/haptic_left",
                "/actions/gameplay/out/haptic_right"
            };
            for (int hand = 0; hand < 2; ++hand)
                if (g_input->GetActionHandle(haptic_paths[hand],
                        &g_handhold_haptic[hand]) != vr::VRInputError_None ||
                    !g_handhold_haptic[hand])
                    log("controls: optional action %s did not resolve",
                        haptic_paths[hand]);
            const char* pose_paths[2] = {
                "/actions/gameplay/in/hand_left",
                "/actions/gameplay/in/hand_right"
            };
            for (int hand = 0; hand < 2; ++hand)
                if (g_input->GetActionHandle(pose_paths[hand],
                        &g_hand_pose[hand]) != vr::VRInputError_None ||
                    !g_hand_pose[hand])
                    log("controls: optional action %s did not resolve",
                        pose_paths[hand]);
            QueryPerformanceFrequency(&g_qpf);
            QueryPerformanceCounter(&g_last);

            log("controls: connected -- %s", path);
            if (config().immersive_controls)
                log("controls: immersive gestures enabled -- chest + left "
                    "grip toggles the personal light; either grip picks "
                    "up the nearer belt item; fast forward grip release "
                    "throws the grapple; trigger plus backward pull yanks "
                    "an attached target; grip near the upper ledge holds "
                    "a hand, sideways pulls shimmy and downward pulls "
                    "request climb-up");
            log_controllers();
            g_ready = true;
            return true;
        }
    }

    namespace
    {
        // The grip pose is a rigid, per-controller-model offset from the raw
        // device pose. Sample both at the same instant ("now", seated space)
        // and hand the offset to vr_session, which applies it to the
        // render-time (WaitGetPoses) raw poses -- same latency as before.
        void measure_grip_offsets()
        {
            if (!g_input || !g_system || config().hand_model != 2)
                return;
            vr::TrackedDevicePose_t raw[vr::k_unMaxTrackedDeviceCount]{};
            g_system->GetDeviceToAbsoluteTrackingPose(
                vr::TrackingUniverseSeated, 0.0f, raw,
                vr::k_unMaxTrackedDeviceCount);
            for (int hand = 0; hand < 2; ++hand)
            {
                if (!g_hand_pose[hand])
                    continue;
                vr::InputPoseActionData_t pose{};
                const vr::EVRInputError error =
                    g_input->GetPoseActionDataRelativeToNow(
                        g_hand_pose[hand], vr::TrackingUniverseSeated, 0.0f,
                        &pose, sizeof(pose),
                        vr::k_ulInvalidInputValueHandle);
                vr::InputOriginInfo_t origin{};
                const bool have_origin = error == vr::VRInputError_None &&
                    pose.bActive && pose.pose.bPoseIsValid &&
                    g_input->GetOriginTrackedDeviceInfo(
                        pose.activeOrigin, &origin, sizeof(origin)) ==
                        vr::VRInputError_None &&
                    origin.trackedDeviceIndex < vr::k_unMaxTrackedDeviceCount;
                if (!have_origin ||
                    !raw[origin.trackedDeviceIndex].bPoseIsValid)
                {
                    if (!g_grip_missing_reported[hand] &&
                        g_frames_in_set > 300 && !g_grip_samples[hand])
                    {
                        g_grip_missing_reported[hand] = true;
                        log("controls: no %s grip pose from SteamVR (error %d, "
                            "active %d) -- that hand keeps the original "
                            "placement. A custom SteamVR binding without the "
                            "\"%s hand grip pose\" action causes this.",
                            hand == 0 ? "left" : "right", (int)error,
                            (int)pose.bActive, hand == 0 ? "Left" : "Right");
                    }
                    continue;
                }
                if (origin.trackedDeviceIndex != g_grip_device[hand])
                {
                    g_grip_device[hand] = origin.trackedDeviceIndex;
                    g_grip_samples[hand] = 0;
                }
                if (g_grip_samples[hand] >= 30)
                    continue; // locked: the offset is rigid
                // Engine convention, metres: P = device -> world.
                const Mat4 grip_view = view_from_pose(
                    &pose.pose.mDeviceToAbsoluteTracking.m[0][0], 1.0f);
                const Mat4 raw_view = view_from_pose(
                    &raw[origin.trackedDeviceIndex]
                        .mDeviceToAbsoluteTracking.m[0][0], 1.0f);
                // grip-local -> raw-local = P_grip * inv(P_raw).
                const Mat4 offset = rigid_inverse(grip_view) * raw_view;
                bool finite = true;
                for (int r = 0; r < 4; ++r)
                    for (int k = 0; k < 4; ++k)
                        finite = finite && std::isfinite(offset.m[r][k]);
                if (!finite)
                    continue;
                if (++g_grip_samples[hand] == 30)
                {
                    vr_set_controller_grip_offset(hand == 0, offset);
                    log("controls: %s grip pose locked: offset (%+.3f, "
                        "%+.3f, %+.3f) m from the raw pose; forward axis "
                        "(%+.2f, %+.2f, %+.2f)", hand == 0 ? "left" : "right",
                        offset.m[3][0], offset.m[3][1], offset.m[3][2],
                        offset.m[2][0], offset.m[2][1], offset.m[2][2]);
                }
            }
        }
    }

    void vr_input_handhold_haptic(bool left)
    {
        const int hand = left ? 0 : 1;
        if (!g_ready || !g_input || !g_handhold_haptic[hand] ||
            !config().immersive_controls || !game_has_focus())
            return;

        // One short tactile cue per zone entry; the draw hook owns the
        // entry/exit latch and never calls this continuously while inside.
        const vr::EVRInputError error = g_input->TriggerHapticVibrationAction(
            g_handhold_haptic[hand], 0.0f, 0.04f, 120.0f, 0.30f,
            vr::k_ulInvalidInputValueHandle);
        if (error != vr::VRInputError_None &&
            !g_handhold_haptic_error_reported[hand])
        {
            g_handhold_haptic_error_reported[hand] = true;
            log("controls: %s handhold haptic failed, EVRInputError %d",
                left ? "left" : "right", (int)error);
        }
    }

    namespace
    {
        void vr_input_handhold_haptic_fwd(bool left)
        {
            vr_input_handhold_haptic(left);
        }
    }

    bool vr_input_gear_cross(VrGearCross* out)
    {
        if (!out || !g_ready || !g_gear_active)
            return false;
        out->visible = true;
        for (int k = 0; k < 3; ++k)
            out->centre[k] = kGearCross[k];
        for (int item = 0; item < 4; ++item)
            gear_item_position(item, out->items[item]);
        out->hover[0] = g_gear_hover[0];
        out->hover[1] = g_gear_hover[1];
        out->handle_hover = g_gear_handle_hover[0] > 0 ||
                            g_gear_handle_hover[1] > 0;
        out->dragging = g_gear_drag_hand >= 0;
        out->flash_item = g_gear_flash_item;
        const ULONGLONG age = GetTickCount64() - g_gear_flash_at;
        out->flash = g_gear_flash_item >= 0 && age < 400
            ? 1.0f - age / 400.0f : 0.0f;
        out->nearness = g_gear_nearness;
        return true;
    }

    bool vr_input_pouch_item(int item, float body[3], int* held_hand)
    {
        if (!g_ready || !body || !held_hand || item < 0 || item > 1 ||
            !config().immersive_controls)
            return false;
        pouch_point(item, body);
        *held_hand = item == 0 ? g_grenade_hand : g_medipack_hand;
        // Holster setup: the pouch item rides its controller until placed.
        const int follow = tune_gear_tuning_hand(item == 0 ? TuneGearGrenade
                                                           : TuneGearMedipack);
        if (follow >= 0)
            *held_hand = follow;
        return true;
    }

    bool vr_input_trigger_held(bool left)
    {
        return g_ready && g_trigger_held[left ? 0 : 1];
    }

    bool vr_input_grip_held(bool left)
    {
        return g_ready && g_grip_held[left ? 0 : 1];
    }

    void vr_input_weapon_haptic(bool left, float strength)
    {
        const int hand = left ? 0 : 1;
        if (!g_ready || !g_input || !g_handhold_haptic[hand] ||
            !config().weapon_haptics || !game_has_focus())
            return;
        // A short, sharp pulse per shot; strength 0..1 scales amplitude and
        // length (pistol lighter, long guns heavier).
        const float s = strength < 0.0f ? 0.0f : strength > 1.0f ? 1.0f
                                                                  : strength;
        g_input->TriggerHapticVibrationAction(
            g_handhold_haptic[hand], 0.0f, 0.03f + 0.04f * s,
            180.0f - 60.0f * s, 0.45f + 0.55f * s,
            vr::k_ulInvalidInputValueHandle);
    }

    void vr_input_update()
    {
        if (!config().controllers || !connect())
            return;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        float dt = (float)(now.QuadPart - g_last.QuadPart) / (float)g_qpf.QuadPart;
        g_last = now;
        // A load, a hitch or a breakpoint must never become a lurch.
        if (dt < 0.0f || dt > 0.1f)
            dt = 0.0f;

        const int index = screen_index();
        const bool menu = index >= 0;
        const short screen = top_screen_id(index);
        if (menu != g_in_menu || index != g_last_index || screen != g_last_screen)
        {
            log("controls: %s  (UI screen stack index %d, top screen id %d)",
                menu ? "menu controls" : "gameplay controls", index, (int)screen);
            g_in_menu = menu;
            g_last_index = index;
            g_last_screen = screen;
        }

        // First gameplay since launch: recentre once, after 0.5 s of real
        // play (no menu, load or cinematic, Lara present), so the view
        // starts facing forward wherever the player stood (user,
        // 2026-10-04).
        {
            static bool recentred = false;
            static ULONGLONG playing_since = 0;
            if (!recentred)
            {
                bool lara = false;
                __try
                {
                    lara = *reinterpret_cast<void* const*>(0x010E537C) !=
                           nullptr;
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    lara = false;
                }
                const bool playing = !menu && lara &&
                    !ui_loading_screen_active() && !camera_cinematic_playing();
                const ULONGLONG t = GetTickCount64();
                if (!playing)
                    playing_since = 0;
                else if (!playing_since)
                    playing_since = t;
                else if (t - playing_since > 500)
                {
                    recentred = true;
                    vr_recenter();
                    log("controls: first gameplay since launch -- view "
                        "recentred once");
                }
            }
        }

        vr::VRActiveActionSet_t active{};
        active.ulActionSet = g_set_game;
        log_set_activity("IVRInput::UpdateActionState");
        const bool updated = g_input->UpdateActionState(&active,
            sizeof(active), 1) == vr::VRInputError_None;
        log_set_activity(nullptr);
        if (!updated)
            return;

        if (++g_frames_in_set == 30 && !g_reported_bindings)
        {
            g_reported_bindings = true;
            report_bound();
        }
        measure_grip_offsets();

        if (!game_has_focus())
        {
            release_all();
            g_holster_grip_was[0] = digital(g_game[GameGrapple].handle);
            g_holster_grip_was[1] = digital(g_game[GameLockOn].handle);
            g_light_grip_was = g_holster_grip_was[0];
            return;
        }

        const bool movie = movie_active();
        if (movie != g_in_movie)
        {
            g_in_movie = movie;
            log("controls: %s movie input", movie ? "entered" : "left");
        }
        if (movie)
        {
            reset_holster_gesture();
            camera_first_person_ledge_grip_input(
                true, digital(g_game[GameGrapple].handle), false);
            camera_first_person_ledge_grip_input(
                false, digital(g_game[GameLockOn].handle), false);
            g_holster_grip_was[0] = digital(g_game[GameGrapple].handle);
            g_holster_grip_was[1] = digital(g_game[GameLockOn].handle);
            g_light_grip_was = g_holster_grip_was[0];
            g_light_key_pulse = false;
            // Enter is the game's proven skip key. During Bink playback the
            // ordinary UI stack can be absent, and the old path emitted a
            // gameplay mouse click or E/Delete from controller triggers.
            // Release gameplay output and route either trigger to Enter.
            bool want[256]{};
            want[VK_RETURN] = digital(g_game[GameFire].handle) ||
                              digital(g_game[GameInteract].handle) ||
                              digital(g_game[GameJump].handle);
            apply(want, false);
            for (bool& d : g_stick_dir) d = false;
            for (bool& d : g_move_dir) d = false;
            g_first_person_jump_held = false;
            for (bool& d : g_nav_dir) d = false;
            g_mouse_rem[0] = g_mouse_rem[1] = 0.0f;
            return;
        }

        const bool immersive_first_person =
            config().immersive_controls && camera_first_person_active();
        // Third-person immersive (branch started 2026-10-01): for now the
        // VR-gamepad button layout in full -- nothing taken away for
        // gestures that need the first-person hands -- plus the body
        // gestures that work without them (chest-grip light). See
        // CONTROLS_IMMERSIVE.md.
        const bool immersive_third_person =
            config().immersive_controls && !camera_first_person_active();
        if (immersive_third_person && !g_tp_reported)
        {
            g_tp_reported = true;
            log("controls: third-person immersive -- VR-gamepad buttons "
                "plus chest-grip light; press log on (first 400 presses)");
        }
        if (immersive_third_person && !menu)
        {
            static const char* names[] = {
                "A jump", "LT interact", "B dive/crouch", "RT fire",
                "left grip grapple", "Y grenade", "X aim",
                "right grip lock-on", "right stick weapon",
                "left stick pause", "left stick long medkit",
                "Y long light", "right stick long binoculars",
                "left stick double recentre", "right stick double tuning",
                "right stick double PDA" };
            for (int i = 0; i < kGameButtons && i < 32; ++i)
            {
                const bool on = digital(g_game[i].handle);
                if (on && !g_tp_button_was[i] &&
                    g_tp_button_reports < 400)
                {
                    ++g_tp_button_reports;
                    float r = 0.0f, u = 0.0f, fw = 0.0f;
                    const bool hand_side = i == GameGrapple ||
                        i == GameInteract || i == GameAim ||
                        i == GameGrenade || i == GameLight ||
                        i == GameMedkit || i == GamePause ||
                        i == GameRecentre;
                    if (vr_controller_body_position(hand_side, &r, &u, &fw))
                        log("controls: [3P] %s pressed (%s hand at right "
                            "%+.2f, up %+.2f, forward %+.2f m)", names[i],
                            hand_side ? "left" : "right", r, u, fw);
                    else
                        log("controls: [3P] %s pressed", names[i]);
                }
                g_tp_button_was[i] = on;
            }
        }

        // Hand calibration, chosen in the pause menu (the Next Generation
        // Content entry, user 2026-10-06). Starts once back in gameplay:
        // Lara's hands are held out in front, pistols drawn in first person,
        // and each grip locks its hand where the controller is.
        if (vr_hand_calibration_menu_pending() && !menu)
        {
            vr_hand_calibration_clear_menu_request();
            g_menu_request_at = 0;
            const bool hands_shown =
                (immersive_first_person &&
                 config().first_person_tracked_hands) ||
                immersive_third_person;
            if (!hands_shown)
            {
                log("controls: hand calibration needs immersive controls "
                    "with tracked hands");
                vr_submit_notice(L"Hand Calibration", L"Hand calibration "
                    L"needs Lara's hands on your controllers: first person "
                    L"with first_person_tracked_hands = 1, or third person.");
            }
            else if (tune_gear_tuning_active())
                log("controls: hand calibration waits for holster setup");
            else if (!vr_hand_calibration_begin())
                vr_submit_notice(L"Hand Calibration", L"Your controllers' "
                    L"grip poses are not available yet. Make sure both "
                    L"controllers are on and tracked, then try again.");
            else
            {
                if (g_binocular_hand >= 0 || g_grapple_hand >= 0 ||
                    g_grenade_hand >= 0 || g_medipack_hand >= 0)
                    reset_holster_gesture();
                release_all(true);
                g_cal_grip_was[0] = digital(g_game[GameGrapple].handle);
                g_cal_grip_was[1] = digital(g_game[GameLockOn].handle);
                g_cal_pause_was = digital(g_game[GamePause].handle);
                g_cal_drew_guns = false;
                if (immersive_first_person && !g_holster_drawn[0] &&
                    !g_holster_drawn[1] && g_binocular_hand < 0 &&
                    g_grapple_hand < 0 &&
                    find_weapon_slot(WeaponPistols) >= 0)
                {
                    bool allowed = false;
                    __try
                    {
                        allowed = reinterpret_cast<PFN_CombatAllowed>(
                            kPlayerCombatAllowed)();
                    }
                    __except(EXCEPTION_EXECUTE_HANDLER)
                    {
                        allowed = false;
                    }
                    if (allowed)
                    {
                        select_weapon_kind(WeaponPistols, false);
                        g_holster_drawn[0] = g_holster_drawn[1] = true;
                        g_pistol_draw_hand = 1;
                        g_pistol_drawn_at = GetTickCount64();
                        reinterpret_cast<PFN_PlayerCombat>(
                            kPlayerInvEnterIndicatorMode)();
                        g_cal_drew_guns = true;
                    }
                    else
                        log("controls: hand calibration without pistols "
                            "(combat not allowed here)");
                }
            }
        }
        if (vr_hand_calibration_active())
        {
            if (menu)
                vr_hand_calibration_cancel("menu opened");
            else
            {
                Button& recentre_button = g_game[GameRecentre];
                const bool recentre_down = digital(recentre_button.handle);
                if (recentre_down && !recentre_button.was)
                    vr_recenter();
                recentre_button.was = recentre_down;
                release_all(true);
                const bool pause_down = digital(g_game[GamePause].handle);
                const bool pause_pressed = pause_down && !g_cal_pause_was;
                g_cal_pause_was = pause_down;
                const bool grips[2] = {
                    digital(g_game[GameGrapple].handle),
                    digital(g_game[GameLockOn].handle)
                };
                if (pause_pressed)
                {
                    vr_hand_calibration_cancel("menu button");
                    g_pause_hold = true;
                }
                for (int hand = 0; hand < 2; ++hand)
                {
                    const bool pressed = grips[hand] && !g_cal_grip_was[hand];
                    g_cal_grip_was[hand] = grips[hand];
                    if (pressed && vr_hand_calibration_active() &&
                        vr_hand_calibration_lock(hand == 0))
                        vr_input_handhold_haptic(hand == 0);
                }
                if (!vr_hand_calibration_active())
                {
                    if (g_cal_drew_guns && g_holster_drawn[0] &&
                        g_holster_drawn[1])
                    {
                        g_holster_drawn[0] = g_holster_drawn[1] = false;
                        g_pistol_draw_hand = -1;
                        g_pistol_drawn_at = 0;
                        reinterpret_cast<PFN_PlayerCombat>(
                            kPlayerInvEndCombatMode)();
                    }
                    g_cal_drew_guns = false;
                    // Grips and triggers still held must not grab or fire.
                    g_holster_grip_was[0] = grips[0];
                    g_holster_grip_was[1] = grips[1];
                    g_light_grip_was = grips[0];
                    g_tuning_trigger_hold[0] =
                        digital(g_game[GameInteract].handle);
                    g_tuning_trigger_hold[1] =
                        digital(g_game[GameFire].handle);
                }
                return;
            }
        }

        // Live first/third-person switch (user request 2026-10-06). Not in
        // menus, setups or calibration; guns and held gear are put away
        // first so neither view inherits the other's hand state.
        {
            const bool view_down = config().immersive_controls &&
                digital(g_game[GameBinoculars].handle);
            const bool view_key = (GetAsyncKeyState(VK_OEM_5) & 1) != 0;
            const bool pressed = (view_down && !g_view_button_was) ||
                                 view_key;
            g_view_button_was = view_down;
            if (pressed && !menu && !tune_gear_tuning_active() &&
                !vr_hand_calibration_active() && !ui_loading_screen_active() &&
                !camera_cinematic_playing())
            {
                reset_holster_gesture();
                release_all();
                camera_toggle_view(view_key ? "\\ key"
                                            : "right stick long press");
                g_view_recenter_at = GetTickCount64() + 250;
            }
            if (g_view_recenter_at && GetTickCount64() >= g_view_recenter_at)
            {
                g_view_recenter_at = 0;
                if (!menu)
                    vr_recenter();
            }
        }

        // Gear tuning. The double-click is its own SteamVR action (F6 on the
        // keyboard), so it never reaches the game as a weapon switch.
        Button& tuning = g_game[GameGearTuning];
        const bool tuning_on = digital(tuning.handle);
        const bool tuning_pressed = (tuning_on && !tuning.was) ||
                                    tune_gear_tuning_take_request();
        tuning.was = tuning_on;
        if (!tune_gear_tuning_active())
            g_menu_tuning = false;
        bool menu_start = false;
        if (tune_gear_tuning_menu_pending() && !menu)
        {
            tune_gear_tuning_clear_menu_request();
            g_menu_request_at = 0;
            if (immersive_first_person && config().first_person_tracked_hands)
            {
                menu_start = true;
                g_menu_tuning = true;
            }
            else
            {
                log("controls: holster setup needs immersive first person "
                    "with tracked hands");
                vr_submit_notice(L"VR Holster Setup", L"Holster setup works "
                    L"in first person with tracked hands. Set first_person = "
                    L"1 and first_person_tracked_hands = 1 in trlvr.ini.");
            }
        }
        const bool tuning_possible =
            (tune_gear_tuning_enabled() || g_menu_tuning) &&
            immersive_first_person && !menu &&
            config().first_person_tracked_hands;
        if (tune_gear_tuning_active() && !tuning_possible)
            tune_gear_tuning_cancel(menu ? "menu opened"
                                         : "left first-person gameplay");
        else if (tuning_pressed && tune_gear_tuning_active())
            tune_gear_tuning_cancel("double-click");
        else if ((tuning_pressed || menu_start) && tuning_possible)
        {
            if (menu_start && (g_holster_drawn[0] || g_holster_drawn[1] ||
                               g_binocular_hand >= 0 || g_grapple_hand >= 0 ||
                               g_grenade_hand >= 0 || g_medipack_hand >= 0))
            {
                // Put the guns away and drop anything held (binoculars,
                // grapple, pouch items) so the placement can start.
                log("controls: holster setup put away the guns / held gear");
                reset_holster_gesture();
            }
            if (g_holster_drawn[0] || g_holster_drawn[1] ||
                g_binocular_hand >= 0 || g_grapple_hand >= 0)
            {
                log("controls: gear tuning needs pistols holstered and no "
                    "belt item in hand");
                vr_submit_notice(L"VR Holster Setup", L"Holster your pistols "
                    L"and put down any item in your hands, then choose VR "
                    L"Holster Setup again.");
            }
            else
            {
                release_all();
                g_tuning_trigger_was[0] =
                    digital(g_game[GameInteract].handle);
                g_tuning_trigger_was[1] = digital(g_game[GameFire].handle);
                g_tuning_pause_was = digital(g_game[GamePause].handle);
                tune_gear_tuning_begin();
            }
        }
        else if (tuning_pressed && tune_gear_tuning_enabled())
            log("controls: gear tuning needs immersive first-person "
                "gameplay with tracked hands");

        if (tune_gear_tuning_active())
        {
            // Every game output stays released; only recentre and the two
            // triggers mean anything. Grips are ignored entirely.
            Button& recentre_button = g_game[GameRecentre];
            const bool recentre_down = digital(recentre_button.handle);
            if (recentre_down && !recentre_button.was)
                vr_recenter();
            recentre_button.was = recentre_down;
            release_all();
            const bool pause_down = digital(g_game[GamePause].handle);
            if (pause_down && !g_tuning_pause_was)
            {
                tune_gear_tuning_cancel("menu button");
                g_pause_hold = true;
            }
            g_tuning_pause_was = pause_down;
            const bool triggers[2] = {
                digital(g_game[GameInteract].handle),
                digital(g_game[GameFire].handle)
            };
            for (int hand = 0; hand < 2; ++hand)
            {
                const bool pressed = triggers[hand] &&
                                     !g_tuning_trigger_was[hand];
                g_tuning_trigger_was[hand] = triggers[hand];
                float body[3]{};
                if (!pressed || !vr_controller_body_position(
                        hand == 0, &body[0], &body[1], &body[2]))
                    continue;
                if (tune_gear_tuning_trigger(hand == 0, body) != 0)
                    vr_input_handhold_haptic(hand == 0);
                if (!tune_gear_tuning_active())
                    break;
            }
            // A trigger still held when tuning ends must not fire or act.
            if (!tune_gear_tuning_active())
            {
                g_tuning_trigger_hold[0] = triggers[0];
                g_tuning_trigger_hold[1] = triggers[1];
                g_holster_grip_was[0] = digital(g_game[GameGrapple].handle);
                g_holster_grip_was[1] = digital(g_game[GameLockOn].handle);
                g_light_grip_was = g_holster_grip_was[0];
            }
            return;
        }
        const bool left_trigger = digital(g_game[GameInteract].handle);
        const bool right_trigger = digital(g_game[GameFire].handle);
        g_grip_held[0] = digital(g_game[GameGrapple].handle);
        g_grip_held[1] = digital(g_game[GameLockOn].handle);
        g_trigger_held[0] = left_trigger && !g_tuning_trigger_hold[0];
        g_trigger_held[1] = right_trigger && !g_tuning_trigger_hold[1];
        g_tuning_trigger_hold[0] = g_tuning_trigger_hold[0] && left_trigger;
        g_tuning_trigger_hold[1] = g_tuning_trigger_hold[1] && right_trigger;
        // Left-handed play swaps the triggers' gameplay roles: the physical
        // left trigger fires and the right one does Action / precision aim.
        // Gear tuning, menus and the grapple pull keep the physical hands.
        const bool swap_triggers = config().left_handed;
        const bool action_trigger = swap_triggers ? right_trigger : left_trigger;
        const int action_hold = swap_triggers ? 1 : 0;
        // Single-pistol mode with pistols (not the long gun) in hand.
        const bool single_pistols_out = config().single_pistols &&
            config().immersive_controls &&
            (g_holster_drawn[0] || g_holster_drawn[1]) &&
            weapon_slot_kind(selected_weapon_slot()) != WeaponLong;

        const bool ledge_hanging = immersive_first_person && !menu &&
            camera_first_person_ledge_hanging();
        const bool bar_hanging = immersive_first_person && !menu &&
            camera_first_person_bar_hanging();
        const bool vine_climbing = immersive_first_person && !menu &&
            camera_first_person_vine_climbing();
        const bool handhold_active = ledge_hanging || bar_hanging ||
            vine_climbing;
        const bool left_grip = digital(g_game[GameGrapple].handle);
        const bool right_grip = digital(g_game[GameLockOn].handle);
        camera_first_person_ledge_grip_input(
            true, left_grip, handhold_active);
        camera_first_person_ledge_grip_input(
            false, right_grip, handhold_active);
        update_holster_gesture(immersive_first_person && !menu &&
                               !handhold_active, menu);
        bool grapple_pull_pulse = false;
        const int grapple_state = update_grapple_pull(
            immersive_first_person && !menu && !handhold_active,
            left_trigger, right_trigger, &grapple_pull_pulse);
        // Resolve the visible belt items and pistol holsters first. Their
        // pickup consumes this grip edge; only an unclaimed upper-chest grip
        // may toggle the personal light.
        g_light_key_pulse = config().immersive_controls && !menu &&
            !handhold_active &&
            left_grip && !g_light_grip_was &&
            !g_holster_grip_consumed[0] &&
            g_binocular_hand < 0 && g_grapple_hand < 0 &&
            left_hand_at_chest();
        g_light_grip_was = left_grip;
        if (g_light_key_pulse)
            g_light_grip_claim = true;
        else if (!left_grip)
            g_light_grip_claim = false;
        const ULONGLONG gear_now = GetTickCount64();
        // Gameplay only: no menus (pause included), loading screens or
        // cutscenes (user, 2026-10-02).
        const bool tp_gameplay = immersive_third_person && !menu &&
            !ui_menu_active() && !ui_loading_screen_active() &&
            !camera_cinematic_playing();
        // Not with first person on: between levels and in loads the
        // first-person view is not active yet, which read as third person
        // and showed the cross (user, 2026-10-04).
        update_gear_cross(tp_gameplay && !camera_view_first_person(), left_grip,
                          right_grip, gear_now);
        // Board mode: a grip around the tiny Lara picks her up (after the
        // gear cross, which keeps any grip on or hovering it).
        bool pluck_claim[2] = { false, false };
        {
            const bool pluck_grip[2] = {
                left_grip && !g_gear_grip_claim[0] && g_gear_hover[0] < 0 &&
                    g_gear_handle_hover[0] < 0,
                right_grip && !g_gear_grip_claim[1] && g_gear_hover[1] < 0 &&
                    g_gear_handle_hover[1] < 0 };
            camera_board_pluck_update(tp_gameplay, pluck_grip, pluck_claim);
            // Y long press (light, freed in third-person immersive): the
            // board-mode finger flick on the left hand.
            camera_board_flick_update(tp_gameplay,
                g_flick_action && digital(g_flick_action));
        }

        bool want[256] = { false };
        bool want_mouse_left = false;

        // Recentring is available in either mode and is edge-triggered.
        Button& recentre = g_game[GameRecentre];
        const bool recentre_on = digital(recentre.handle);
        if (recentre_on && !recentre.was)
            vr_recenter();
        recentre.was = recentre_on;

        if (!menu)
        {
            for (int i = 0; i < kGameButtons; ++i)
            {
                Button& b = g_game[i];
                if (b.out == RECENTRE || b.out == GEAR_TUNING)
                    continue;
                if ((i == GameInteract &&
                     g_tuning_trigger_hold[swap_triggers ? 1 : 0]) ||
                    (i == GameFire &&
                     g_tuning_trigger_hold[swap_triggers ? 0 : 1]))
                    continue;
                // Immersive mode routes LT contextually below and the chest
                // grip to light. The old light action remains unused here.
                // Third person keeps every button (no hands to replace them).
                if (immersive_first_person &&
                    (i == GameInteract || i == GameLight))
                    continue;
                if (immersive_first_person &&
                    (i == GameGrapple || i == GameBinoculars))
                    continue;
                // The left grip that just toggled the chest light.
                if (i == GameGrapple && g_light_grip_claim)
                    continue;
                // Third-person immersive: the 3D gear cross does these, so
                // their buttons are left free (user, 2026-10-01): right
                // stick click (weapon switch) and long press (binoculars),
                // left stick long press (medipack), Y long press (light).
                if (immersive_third_person &&
                    (i == GameWeapon || i == GameBinoculars ||
                     i == GameMedkit || i == GameLight))
                    continue;
                if (i == GamePause && g_pause_hold)
                {
                    g_pause_hold = digital(g_game[GamePause].handle);
                    continue;
                }
                // A grip that grabbed from the gear cross, or holds Lara.
                if ((i == GameGrapple && (g_gear_grip_claim[0] ||
                                          pluck_claim[0])) ||
                    (i == GameLockOn && (g_gear_grip_claim[1] ||
                                         pluck_claim[1])))
                    continue;
                // While Lara is held she neither jumps nor rolls.
                // On a palm she may jump and crouch (2026-10-04).
                if (camera_board_pluck_carried() &&
                    (i == GameJump || i == GameDive))
                    continue;
                // Right grip drives spatial gestures in immersive first person.
                // Sending its retail lock-on key outside a gesture zone can
                // end pistol combat even though the hand is nowhere near a holster.
                if (immersive_first_person && i == GameLockOn)
                    continue;
                // X (aim) drew weapons straight into retail combat, which
                // bypasses the hip-grip holster state: the guns then floated
                // at the hidden body's hands. Immersive play draws at the
                // holster instead; non-immersive keeps X.
                if (immersive_first_person && i == GameAim)
                    continue;
                // A grip that is steadying the long gun is not also the
                // retail grapple / lock-on key.
                if ((i == GameGrapple && camera_two_hand_grip_claims(true)) ||
                    (i == GameLockOn && camera_two_hand_grip_claims(false)))
                    continue;
                if (immersive_first_person &&
                    i == GameFire &&
                    (!g_holster_drawn[0] && !g_holster_drawn[1] ||
                     grapple_state != 0))
                    continue;
                if (immersive_first_person &&
                    i == GameGrapple && g_holster_grip_consumed[0])
                    continue;
                bool on = digital(
                    swap_triggers && i == GameFire
                        ? g_game[GameInteract].handle
                    : swap_triggers && i == GameInteract
                        ? g_game[GameFire].handle
                        : b.handle);
                // Single pistols: each drawn pistol's own trigger fires.
                if (single_pistols_out && i == GameFire)
                    on = (left_trigger && g_holster_drawn[0] &&
                          !g_tuning_trigger_hold[0]) ||
                         (right_trigger && g_holster_drawn[1] &&
                          !g_tuning_trigger_hold[1]);
                if (b.out == KEY && on)
                    want[b.vk] = true;
                else if (b.out == MOUSE_LEFT && on)
                    want_mouse_left = true;
            }

            if (immersive_first_person)
            {
                // A trigger whose hand holds a single-mode pistol fires it
                // instead of doing Action / precision aim.
                if (action_trigger && !g_tuning_trigger_hold[action_hold] &&
                    !(single_pistols_out && g_holster_drawn[action_hold]))
                {
                    // X and LT now use exactly the same held Z output. The
                    // game edge-detects Z as an accurate-aim toggle; sending
                    // another pulse on LT release toggled it straight back.
                    if (immersive_first_person &&
                        (g_holster_drawn[0] || g_holster_drawn[1]))
                        want['Z'] = true;
                    else if (grapple_state == 0)
                        want['E'] = true;
                }
                // Precision aim is a toggle on Z, and in first person it
                // barely shows: one stray trigger left Lara rooted with only
                // turning working (log 2026-10-04, camera mode 13 from 41.8
                // to 60 s). Pushing the left stick with the trigger released
                // toggles it off again.
                {
                    static ULONGLONG move_since = 0;
                    static ULONGLONG z_until = 0;
                    static ULONGLONG last_exit = 0;
                    const ULONGLONG t = GetTickCount64();
                    const bool moving = g_move_dir[0] || g_move_dir[1] ||
                                        g_move_dir[2] || g_move_dir[3];
                    if (moving && !action_trigger &&
                        camera_accurate_aim_active())
                    {
                        if (!move_since)
                            move_since = t;
                        if (t - move_since > 300 && t - last_exit > 1000)
                        {
                            z_until = t + 100;
                            last_exit = t;
                            log("controls: left stick while precision aim "
                                "was on -- precision aim toggled off");
                        }
                    }
                    else
                        move_since = 0;
                    if (t < z_until)
                        want['Z'] = true;
                }
                if (grapple_pull_pulse)
                    want['E'] = true;
                if (camera_first_person_secure_grip_pulse())
                    want['E'] = true; // grab secures a precarious catch
            }
            if (config().immersive_controls)
            {
                if (g_light_key_pulse)
                {
                    want[VK_DELETE] = true;
                    if (!g_reported_immersive_light)
                    {
                        g_reported_immersive_light = true;
                        log("controls: first immersive chest + left grip gesture "
                            "sent as personal light");
                    }
                }
            }
            if (immersive_first_person)
            {
                if (g_binocular_key_pulse)
                    want[VK_NEXT] = true;
                const bool armed = g_holster_drawn[0] || g_holster_drawn[1];
                if (armed && camera_grapple_has_target())
                    g_grapple_target_seen_at = GetTickCount64();
                if (g_grapple_throw_pulse)
                {
                    // With a gun out, retail only throws the grapple at
                    // an enemy it has targeted (log 2026-10-04). With no
                    // target the gun is put away for the throw, so the hook
                    // flies as it does holstered, and drawn again below
                    // unless the hook latches onto something.
                    const ULONGLONG t = GetTickCount64();
                    g_grapple_key_from = t;
                    if (armed && t - g_grapple_target_seen_at > 400)
                    {
                        g_regrab_pending = true;
                        g_regrab_deployed = false;
                        g_regrab_drawn[0] = g_holster_drawn[0];
                        g_regrab_drawn[1] = g_holster_drawn[1];
                        g_regrab_kind = weapon_slot_kind(selected_weapon_slot());
                        g_regrab_hand = g_pistol_draw_hand;
                        g_regrab_since = t;
                        g_holster_drawn[0] = g_holster_drawn[1] = false;
                        g_pistol_draw_hand = -1;
                        g_pistol_drawn_at = 0;
                        reinterpret_cast<PFN_PlayerCombat>(
                            kPlayerInvEndCombatMode)();
                        g_grapple_key_from = t + 120;
                        log("controls: grapple thrown with a gun out and no "
                            "target -- gun holstered for the throw");
                    }
                    g_grapple_key_until = g_grapple_key_from + 150;
                }
                {
                    const ULONGLONG t = GetTickCount64();
                    if (t >= g_grapple_key_from && t < g_grapple_key_until)
                        want['Q'] = true;
                }
                if (g_regrab_pending)
                {
                    const ULONGLONG t = GetTickCount64();
                    const int hook = camera_first_person_grapple_state();
                    if (g_holster_drawn[0] || g_holster_drawn[1])
                        g_regrab_pending = false; // drawn by hand meanwhile
                    else if (hook == 2)
                    {
                        g_regrab_pending = false;
                        log("controls: grapple latched on -- guns stay "
                            "holstered");
                    }
                    else if (hook == 1)
                        g_regrab_deployed = true;
                    else if ((g_regrab_deployed && t - g_regrab_since > 300) ||
                             t - g_regrab_since > 2500)
                    {
                        g_regrab_pending = false;
                        bool allowed = false;
                        __try
                        {
                            allowed = reinterpret_cast<PFN_CombatAllowed>(
                                kPlayerCombatAllowed)();
                        }
                        __except(EXCEPTION_EXECUTE_HANDLER)
                        {
                            allowed = false;
                        }
                        if (allowed)
                        {
                            const int kind = g_regrab_kind == WeaponLong
                                ? WeaponLong : WeaponPistols;
                            select_weapon_kind(kind, false);
                            // The grapple left the requested slot (+0x3E5)
                            // on the pistols, so retail swapped the long gun
                            // back to them (user, 2026-10-04).
                            const int slot = find_weapon_slot(kind);
                            __try
                            {
                                unsigned char* pd =
                                    *reinterpret_cast<unsigned char**>(
                                        kPlayerDataPointer);
                                if (pd && slot >= 0)
                                    pd[0x3E5] =
                                        static_cast<unsigned char>(slot);
                            }
                            __except(EXCEPTION_EXECUTE_HANDLER)
                            {
                            }
                            g_holster_drawn[0] = g_regrab_drawn[0];
                            g_holster_drawn[1] = g_regrab_drawn[1];
                            g_pistol_draw_hand = g_regrab_hand;
                            g_pistol_drawn_at = t;
                            reinterpret_cast<PFN_PlayerCombat>(
                                kPlayerInvEnterIndicatorMode)();
                        }
                        log("controls: grapple caught nothing -- %s",
                            allowed ? "guns drawn again"
                                    : "combat not allowed, guns stay away");
                    }
                }
                if (g_grenade_throw_pulse)
                    want['K'] = true;
                if (g_medipack_pulse)
                    want[VK_HOME] = true;
            }
            if (g_gear_pulse_key && gear_now < g_gear_pulse_until)
                want[g_gear_pulse_key] = true;
            g_binocular_key_pulse = false;
            g_grapple_throw_pulse = false;
            g_grenade_throw_pulse = false;
            g_medipack_pulse = false;
            g_light_key_pulse = false;
        }

        float x = 0.0f, y = 0.0f;
        if (menu)
        {
            // A grip held while leaving a menu must be released before it
            // can start the chest light gesture in gameplay.
            g_light_key_pulse = false;
            analog(g_move, &x, &y);
            g_nav_dir[0] = latch(g_nav_dir[0],  y, 0.6f, 0.35f);
            g_nav_dir[1] = latch(g_nav_dir[1], -x, 0.6f, 0.35f);
            g_nav_dir[2] = latch(g_nav_dir[2], -y, 0.6f, 0.35f);
            g_nav_dir[3] = latch(g_nav_dir[3],  x, 0.6f, 0.35f);
            const WORD keys[4] = { VK_UP, VK_LEFT, VK_DOWN, VK_RIGHT };
            for (int i = 0; i < 4; ++i)
                if (g_nav_dir[i])
                    want[keys[i]] = true;

            // Reuse the normal gameplay bindings so shipped or cached SteamVR
            // layouts work in menus without a separately bound action set.
            if (digital(g_game[GameJump].handle) ||
                digital(g_game[GameFire].handle))
                want[VK_RETURN] = true;
            if (digital(g_game[GameDive].handle) ||
                digital(g_game[GamePause].handle))
                want[VK_ESCAPE] = true;
            // Holster setup chosen in the pause menu: back out of it (Esc
            // held 100 ms every 450 ms) so placement starts in gameplay.
            // Hand calibration backs out the same way.
            if (tune_gear_tuning_menu_pending() ||
                vr_hand_calibration_menu_pending())
            {
                const ULONGLONG t = GetTickCount64();
                if (!g_menu_request_at)
                    g_menu_request_at = t;
                const ULONGLONG since = t - g_menu_request_at;
                if (since > 6000)
                {
                    tune_gear_tuning_clear_menu_request();
                    vr_hand_calibration_clear_menu_request();
                    g_menu_request_at = 0;
                    log("controls: holster setup / hand calibration dropped "
                        "(menu did not close)");
                }
                else if (since > 200 && (since - 200) % 450 < 100)
                    want[VK_ESCAPE] = true;
            }
            for (bool& d : g_stick_dir) d = false;
            for (bool& d : g_move_dir) d = false;
            g_fast_shimmy = FastShimmyState{};
            g_fast_climb = FastShimmyState{};
            g_vine_look_side = 0;
            g_first_person_jump_held = false;
            g_mouse_rem[0] = g_mouse_rem[1] = 0.0f;
        }
        else
        {
            g_first_person_jump_held = digital(g_game[GameJump].handle);
            analog(g_move, &x, &y);
            g_move_raw[0] = x;
            g_move_raw[1] = y;
            g_stick_dir[0] = latch(g_stick_dir[0],  y, 0.45f, 0.30f);
            g_stick_dir[1] = latch(g_stick_dir[1], -x, 0.45f, 0.30f);
            g_stick_dir[2] = latch(g_stick_dir[2], -y, 0.45f, 0.30f);
            g_stick_dir[3] = latch(g_stick_dir[3],  x, 0.45f, 0.30f);
            for (int i = 0; i < 4; ++i)
                g_move_dir[i] = g_stick_dir[i];
            float ledge_pull = 0.0f;
            float ledge_pull_up = 0.0f;
            HandholdPullSample hand_pulls[2]{};
            const bool pulling_handhold = handhold_active &&
                camera_first_person_ledge_pull(&ledge_pull,
                                               &ledge_pull_up,
                                               hand_pulls);
            if (pulling_handhold && vine_climbing)
            {
                g_fast_shimmy = FastShimmyState{};
                g_vine_look_side = 0;
                // 2026-10-05: a pull up or down that drifts sideways no
                // longer adds a sideways move (on a chain that turned Lara
                // round it): sideways counts only when it clearly
                // dominates. The vertical pull wins otherwise.
                if (fabsf(ledge_pull) < 1.3f * fabsf(ledge_pull_up))
                    ledge_pull = 0.0f;
                // On an attached vine, ladder or vertical pole, the latest
                // anchored hand drives the retail up/down climbing keys.
                // Sideways pull uses the same hand gesture as ledge shimmy.
                // A free hand can reach for the next hold. The stick resumes
                // as soon as both hands release.
                g_vine_pull_up = latch(g_vine_pull_up,
                                       ledge_pull_up, 0.12f, 0.04f);
                g_vine_pull_down = latch(g_vine_pull_down,
                                         -ledge_pull_up, 0.12f, 0.04f);
                g_vine_pull_left = latch(g_vine_pull_left,
                                         -ledge_pull, 0.10f, 0.04f);
                g_vine_pull_right = latch(g_vine_pull_right,
                                          ledge_pull, 0.10f, 0.04f);
                const int climb = climb_hand_over_hand(
                    hand_pulls, int(g_vine_pull_up) - int(g_vine_pull_down));
                g_move_dir[0] = climb > 0;
                g_move_dir[2] = climb < 0;
                g_move_dir[1] = g_vine_pull_left;
                g_move_dir[3] = g_vine_pull_right;
                // Hold Action (fast traversal) while hands alternate, only
                // where the game allows it.
                if (g_fast_climb.active &&
                    (fast_traversal_flags() & 1u) != 0)
                    want['E'] = true;
                const int direction = climb;
                if (direction != g_last_vine_pull_dir &&
                    g_vine_pull_reports < 128)
                {
                    log("controls: vertical hand pull %+.2f m -> %s",
                        ledge_pull_up, direction > 0 ? "up" :
                        direction < 0 ? "down" : "stop");
                    ++g_vine_pull_reports;
                }
                g_last_vine_pull_dir = direction;
                const int side = int(g_vine_pull_right) -
                                 int(g_vine_pull_left);
                if (side != g_last_vine_pull_side &&
                    g_vine_pull_reports < 128)
                {
                    log("controls: vertical hold lateral pull %+.2f m -> %s",
                        ledge_pull, side > 0 ? "right" :
                        side < 0 ? "left" : "stop");
                    ++g_vine_pull_reports;
                }
                g_last_vine_pull_side = side;
                g_last_ledge_pull_dir = 0;
                g_last_ledge_pull_up = false;
                g_ledge_pull_left = false;
                g_ledge_pull_right = false;
                g_ledge_pull_up = false;
            }
            else if (pulling_handhold && (ledge_hanging || bar_hanging))
            {
                g_fast_climb = FastShimmyState{};
                g_vine_look_side = 0;
                g_vine_pull_up = false;
                g_vine_pull_down = false;
                g_vine_pull_left = false;
                g_vine_pull_right = false;
                g_last_vine_pull_dir = 0;
                g_last_vine_pull_side = 0;
                // A held hand stays at its world anchor. Pulling the
                // controller to the left moves Lara to the right (and vice
                // versa). The game's own A/D shimmy keeps ledge collision,
                // corners and animation authoritative. Hysteresis prevents
                // key chatter as the body catches up to the anchor.
                g_ledge_pull_left = latch(g_ledge_pull_left,
                                          -ledge_pull, 0.10f, 0.04f);
                g_ledge_pull_right = latch(g_ledge_pull_right,
                                           ledge_pull, 0.10f, 0.04f);
                // Both anchored hands pulling down request Up plus the
                // controller A/jump action (Space in the game's keyboard
                // path). On a bar, the left stick's forward/back input must
                // remain available for the separate swing action.
                g_ledge_pull_up = ledge_hanging &&
                    fabsf(ledge_pull) < 0.15f &&
                    latch(g_ledge_pull_up, ledge_pull_up, 0.22f, 0.08f);
                g_move_dir[0] = bar_hanging
                    ? g_stick_dir[0] : g_ledge_pull_up;
                g_move_dir[2] = bar_hanging && g_stick_dir[2];
                g_move_dir[1] = !g_ledge_pull_up && g_ledge_pull_left;
                g_move_dir[3] = !g_ledge_pull_up && g_ledge_pull_right;
                if (ledge_hanging && g_ledge_pull_up &&
                    !g_last_ledge_pull_up)
                {
                    g_ledge_pull_requested_at = GetTickCount();
                    g_ledge_jump_until = GetTickCount() + 100;
                }
                if (g_ledge_pull_up &&
                    (int)(g_ledge_jump_until - GetTickCount()) > 0)
                    want[VK_SPACE] = true;
                if (g_ledge_pull_up != g_last_ledge_pull_up &&
                    g_ledge_pull_reports < 128)
                {
                    log("controls: ledge vertical pull %+.2f m -> %s",
                        ledge_pull_up,
                        g_ledge_pull_up ? "climb Up+Jump" : "stop");
                    ++g_ledge_pull_reports;
                }
                g_last_ledge_pull_up = g_ledge_pull_up;
                int direction = g_ledge_pull_up ? 0 :
                    int(g_move_dir[3]) - int(g_move_dir[1]);
                if (!g_ledge_pull_up)
                {
                    direction = hand_over_hand(hand_pulls, direction,
                                               bar_hanging);
                    g_move_dir[1] = direction < 0;
                    g_move_dir[3] = direction > 0;
                    // Hold Action (the game's LT fast-traversal button)
                    // while hands alternate.
                    if (g_fast_shimmy.active)
                        want['E'] = true;
                }
                else
                    g_fast_shimmy = FastShimmyState{};
                if (direction != g_last_ledge_pull_dir &&
                    g_ledge_pull_reports < 128)
                {
                    log("controls: %s pull %+.2f m -> %s",
                        bar_hanging ? "swing bar" : "ledge",
                        ledge_pull, direction < 0 ? "left" :
                        direction > 0 ? "right" : "stop");
                    ++g_ledge_pull_reports;
                }
                g_last_ledge_pull_dir = direction;
            }
            else
            {
                g_fast_shimmy = FastShimmyState{};
                // A vertical climb keeps its direction through the moment
                // both hands are off (hand swap); otherwise it is reset.
                bool climb_carried = false;
                if (vine_climbing)
                {
                    HandholdPullSample none[2]{};
                    const int climb = climb_hand_over_hand(none, 0);
                    if (climb != 0 && !g_move_dir[0] && !g_move_dir[2] &&
                        !g_move_dir[1] && !g_move_dir[3])
                    {
                        g_move_dir[0] = climb > 0;
                        g_move_dir[2] = climb < 0;
                        climb_carried = true;
                    }
                }
                else
                    g_fast_climb = FastShimmyState{};
                g_last_ledge_pull_dir = 0;
                g_last_ledge_pull_up = false;
                g_ledge_pull_left = false;
                g_ledge_pull_right = false;
                g_ledge_pull_up = false;
                g_vine_pull_up = false;
                g_vine_pull_down = false;
                g_vine_pull_left = false;
                g_vine_pull_right = false;
                g_last_vine_pull_dir = 0;
                g_last_vine_pull_side = 0;
                // Only a forward stick on an unheld vertical handhold is
                // redirected. Direct left/right stick input, backward input,
                // attached jumps and controller hand pulls retain their
                // existing retail bindings.
                if (vine_climbing && !g_first_person_jump_held &&
                    !climb_carried && !camera_first_person_free_pole())
                {
                    float look = 0.0f;
                    int side = 0;
                    if (camera_first_person_vine_look_angle(&look))
                    {
                        const float absolute = fabsf(look);
                        const int sign = look < 0.0f ? -1 : 1;
                        // Enter at 30 degrees so a short right-stick turn
                        // reaches the side sector; stay there down to 20.
                        // Beyond 135 degrees is a backward view.
                        if (absolute < 2.35619449f &&
                            (absolute >= 0.52359878f ||
                             (sign == g_vine_look_side &&
                              absolute >= 0.34906585f)))
                            side = sign;
                        if (side != g_vine_look_side &&
                            g_vine_look_reports < 64)
                        {
                            log("controls: vine view %+.1f deg -> forward %s (%s)",
                                look * 57.2957795f,
                                side < 0 ? "left" :
                                side > 0 ? "right" : "up",
                                side < 0 ? "D" : side > 0 ? "A" : "W");
                            ++g_vine_look_reports;
                        }
                    }
                    g_vine_look_side = side;
                    if (g_move_dir[0] &&
                        !g_move_dir[1] && !g_move_dir[3] && side != 0)
                    {
                        g_move_dir[0] = false;
                        // Vine lateral keys are reversed relative to the
                        // avatar's view: the prior build sent D for a
                        // rightward look and the headset test moved left.
                        g_move_dir[side < 0 ? 3 : 1] = true;
                    }
                }
                else
                    g_vine_look_side = 0;
            }
            const unsigned move_mask =
                (g_move_dir[0] ? 1u : 0u) |
                (g_move_dir[1] ? 2u : 0u) |
                (g_move_dir[2] ? 4u : 0u) |
                (g_move_dir[3] ? 8u : 0u);
            // Keep directional transitions in the headset log long enough to
            // diagnose backward/strafe travel after extended forward play.
            if (move_mask != g_last_move_mask && g_move_reports < 256)
            {
                log("controls: movement (stick %.2f, %.2f%s) -> %s%s%s%s",
                    x, y, pulling_handhold ? ", hand pull" : "",
                    g_move_dir[0] ? "W" : "",
                    g_move_dir[1] ? "A" : "",
                    g_move_dir[2] ? "S" : "",
                    g_move_dir[3] ? "D" : "");
                ++g_move_reports;
            }
            g_last_move_mask = move_mask;
            // Lara held in board mode: she does not walk (on a palm she
            // may).
            if (camera_board_pluck_carried())
                for (bool& d : g_move_dir)
                    d = false;
            // Binocular view: the left stick zooms instead of moving (the
            // PC zoom keys are H in / G out, beside the mouse wheel).
            {
                static int zoom_was = 0;
                int zoom = 0;
                if (hud_binocular_view_active())
                {
                    zoom = g_move_raw[1] >= 0.5f ? 1
                         : g_move_raw[1] <= -0.5f ? -1 : 0;
                    for (bool& d : g_move_dir)
                        d = false;
                    if (zoom > 0)
                        want['H'] = true;
                    else if (zoom < 0)
                        want['G'] = true;
                }
                if (zoom != zoom_was)
                {
                    static unsigned reports = 0;
                    if (reports++ < 32)
                        log("controls: binocular zoom %s", zoom > 0
                            ? "in (H)" : zoom < 0 ? "out (G)" : "stop");
                    zoom_was = zoom;
                }
            }
            const WORD keys[4] = { 'W', 'A', 'S', 'D' };
            for (int i = 0; i < 4; ++i)
                if (g_move_dir[i])
                    want[keys[i]] = true;
            for (bool& d : g_nav_dir) d = false;

            // The look stick as a mouse: pixels per second at full
            // deflection, integrated with the remainder carried so slow
            // movements are not lost to rounding.
            analog(g_look, &x, &y);
            const float speed = tune_look_speed();
            if (camera_first_person_active())
            {
                // Independent first-person heading. Letting the third-person
                // camera own yaw made strafing and jumps swing the headset.
                // One full stick gives 120 degrees/second, with the existing
                // fine-control dead zone and response curve.
                if (config().snap_turn)
                {
                    // One step per push past 0.6, re-armed under 0.3.
                    if (g_snap_turn_armed && fabsf(x) >= 0.6f)
                    {
                        const float step = config().snap_turn_degrees *
                                           0.0174532925f;
                        g_first_person_turn -= x > 0.0f ? step : -step;
                        g_snap_turn_armed = false;
                    }
                    else if (fabsf(x) < 0.3f)
                        g_snap_turn_armed = true;
                }
                else
                    g_first_person_turn -= shape(x) * 2.0943951f * dt;
                g_mouse_rem[0] = 0.0f;
            }
            else
                g_mouse_rem[0] += shape(x) * speed * dt;
            if (config().look_vertical)
                g_mouse_rem[1] -= shape(y) * speed * dt;
            const LONG dx = (LONG)g_mouse_rem[0];
            const LONG dy = (LONG)g_mouse_rem[1];
            g_mouse_rem[0] -= (float)dx;
            g_mouse_rem[1] -= (float)dy;
            if (dx || dy)
            {
                send_mouse(MOUSEEVENTF_MOVE, dx, dy);
                note_output("mouse movement");
            }
        }

        // The two-hand pull begins retail LedgeClimb, but its grip-based W
        // stops as soon as the hang state releases. Carry only Up through
        // that specific animation; do not repeat Jump or push after landing.
        const bool ledge_climb = immersive_first_person && !menu &&
            camera_first_person_ledge_climbing();
        const DWORD input_ticks = GetTickCount();
        if (!immersive_first_person || menu)
        {
            g_ledge_pull_requested_at = 0;
            g_ledge_climb_followthrough = false;
        }
        else if (ledge_climb)
        {
            if (!g_ledge_climb_followthrough &&
                g_ledge_pull_requested_at &&
                input_ticks - g_ledge_pull_requested_at <= 1000)
            {
                g_ledge_climb_followthrough = true;
                log("controls: two-hand pull entered LedgeClimb; "
                    "holding Up through the mantle");
            }
            if (g_ledge_climb_followthrough)
            {
                if (want['S'])
                {
                    g_ledge_climb_followthrough = false;
                    g_ledge_pull_requested_at = 0;
                    log("controls: ledge climb follow-through cancelled "
                        "by backward input");
                }
                else
                    want['W'] = true;
            }
        }
        else if (g_ledge_climb_followthrough)
        {
            g_ledge_climb_followthrough = false;
            g_ledge_pull_requested_at = 0;
            log("controls: ledge climb follow-through ended at state exit");
        }
        else if (!ledge_hanging && g_ledge_pull_requested_at &&
                 input_ticks - g_ledge_pull_requested_at > 1000)
            g_ledge_pull_requested_at = 0;

        // Physical crouch holds the crouch key (F, the "dive" action: a
        // crouch when standing, a roll when running -- so it only starts
        // with the move stick near centre). Hysteresis 0.35 / 0.25 m below
        // the height first person started at. The camera takes the deeper
        // of the real and animated crouch, so pressing the button as well
        // does not double the drop. If retail treats crouch as a toggle
        // and she stays down after standing up, one pulse stands her up.
        {
            const float scale = tune_world_scale();
            const float drop = scale > 0.0f ? vr_head_drop() / scale : 0.0f;
            const bool eligible = config().physical_crouch &&
                immersive_first_person && !menu &&
                camera_first_person_grounded();
            const float stick = sqrtf(g_move_raw[0] * g_move_raw[0] +
                                      g_move_raw[1] * g_move_raw[1]);
            const ULONGLONG t = GetTickCount64();
            if (!eligible)
            {
                if (g_physical_crouch)
                    log("controls: physical crouch released (context)");
                g_physical_crouch = false;
                g_physical_stand_at = 0;
            }
            else if (!g_physical_crouch && drop >= 0.35f && stick < 0.3f)
            {
                g_physical_crouch = true;
                g_physical_stand_at = 0;
                log("controls: physical crouch (headset %.2f m down) -> "
                    "crouch held", drop);
            }
            else if (g_physical_crouch && drop < 0.25f)
            {
                g_physical_crouch = false;
                g_physical_stand_at = t;
                log("controls: physical crouch ended (headset %.2f m down)",
                    drop);
            }
            if (g_physical_crouch)
                want['F'] = true;
            // Toggle safeguard: still crouched 0.5 s after standing up, and
            // the crouch button is not held -> one 100 ms pulse.
            if (g_physical_stand_at && eligible &&
                t - g_physical_stand_at >= 500)
            {
                if (camera_first_person_crouched() &&
                    !digital(g_game[GameDive].handle))
                {
                    if (t - g_physical_stand_at < 600)
                        want['F'] = true;
                    else
                    {
                        log("controls: Lara stayed crouched after a physical "
                            "stand; crouch pulsed to stand");
                        g_physical_stand_at = 0;
                    }
                }
                else
                    g_physical_stand_at = 0;
            }
        }

        apply(want, want_mouse_left);
    }

    bool vr_input_physical_crouch_held()
    {
        return g_physical_crouch;
    }

    VrControllerStyle vr_input_controller_style()
    {
        return g_controller_style;
    }

    bool vr_input_holster_hand_drawn(bool left)
    {
        return config().immersive_controls &&
            g_holster_drawn[left ? 0 : 1];
    }

    short vr_input_top_screen_id()
    {
        return g_in_menu ? g_last_screen : (short)-1;
    }

    bool vr_input_holster_combat_held()
    {
        return config().immersive_controls &&
            (g_holster_drawn[0] || g_holster_drawn[1]);
    }

    int vr_input_holster_visual_state(bool left)
    {
        const int hand = left ? 0 : 1;
        if (g_holster_drawn[hand]) return 3;
        if (g_holster_inside[hand]) return 1;
        return 0;
    }

    void vr_input_pistol_combat_ended(uintptr_t caller)
    {
        if (!g_holster_drawn[0] && !g_holster_drawn[1])
            return;
        g_holster_drawn[0] = g_holster_drawn[1] = false;
        g_pistol_draw_hand = -1;
        g_pistol_drawn_at = 0;
        log("controls: retail combat ended (caller %p); cleared VR pistol "
            "draw state", reinterpret_cast<void*>(caller));
    }

    float vr_input_first_person_turn()
    {
        return g_first_person_turn;
    }

    bool vr_input_bar_fast_traverse()
    {
        return g_fast_shimmy.active;
    }

    bool vr_input_first_person_move(float* right, float* forward)
    {
        if (!right || !forward)
            return false;
        *right = float(g_move_dir[3]) - float(g_move_dir[1]);
        *forward = float(g_move_dir[0]) - float(g_move_dir[2]);
        return *right != 0.0f || *forward != 0.0f;
    }

    bool vr_input_first_person_stick(float* right, float* forward)
    {
        if (!right || !forward)
            return false;
        *right = g_move_raw[0];
        *forward = g_move_raw[1];
        return true;
    }

    bool vr_input_first_person_jump_held()
    {
        return g_first_person_jump_held;
    }
}
