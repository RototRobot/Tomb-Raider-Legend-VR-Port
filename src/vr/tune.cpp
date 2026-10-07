#include "tune.h"
#include "../proxy/gpu_profile.h"
#include "cull.h"
#include "hud_capture.h"

#include "vr_session.h"
#include "camera_head.h"
#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>
#include <cmath>
#include <cstdlib>
#include <cwchar>

namespace trlvr
{
    namespace
    {
        float g_hand_offset[2][3]{};
        float g_hand_rotation[2][3]{};
        // hand_model 2: ONE hand pose (the right; the left mirrors it).
        // Position in metres and rotation in degrees, both in the grip
        // pose's own axes (engine: x right, y down, z forward), about the
        // palm. grip_hand_cal is the base rotation converted once from the
        // original model; grip_aim_* the converted aim ray.
        float g_grip_pos[3]{};
        float g_grip_rot[3]{};
        float g_grip_cal[9]{};
        bool g_grip_cal_valid = false;
        float g_grip_aim_pitch = 0.0f;
        float g_grip_aim_yaw = 0.0f;
        bool g_grip_aim_valid = false;
        const wchar_t* kGripPosKeys[3] = {
            L"grip_hand_x", L"grip_hand_y", L"grip_hand_z" };
        const wchar_t* kGripRotKeys[3] = {
            L"grip_hand_rot_x", L"grip_hand_rot_y", L"grip_hand_rot_z" };
        const wchar_t* kGripCalKeys[9] = {
            L"grip_hand_cal_m00", L"grip_hand_cal_m01", L"grip_hand_cal_m02",
            L"grip_hand_cal_m10", L"grip_hand_cal_m11", L"grip_hand_cal_m12",
            L"grip_hand_cal_m20", L"grip_hand_cal_m21", L"grip_hand_cal_m22" };
        bool g_hand_offset_loaded = false;
        VrHolsterZone g_holster_zone = vr_default_holster_zone();
        float g_first_person_belt_height = 12.0f;
        float g_ledge_grab_height = 0.12f;
        bool g_ledge_grab_debug_draw = false;
        float g_grip_prompt_scale = 0.25f;
        // Controller aim-ray calibration, degrees about the controller's own
        // axes: positive pitch raises the ray, positive yaw turns it right.
        float g_aim_pitch = 0.0f;
        float g_aim_yaw = 0.0f;
        // Left-handed aim (aim_pitch_left / aim_yaw_left). Until saved they
        // default to the mirror of the right-hand values: same pitch,
        // negated yaw.
        float g_aim_pitch_left = 0.0f;
        float g_aim_yaw_left = 0.0f;
        // [developer] aim_tuning_debug: laser along the aim ray and numpad
        // pitch/yaw nudges. Takes the numpad from hand tuning while on.
        bool g_aim_debug_enabled = false;
        bool g_aim_key_down[6]{};
        ULONGLONG g_aim_key_repeat_at[6]{};
        // -1 until the INI default is read; F7 then flips it for this run.
        int g_view_prescale_fix = -1;
        unsigned g_ledge_visibility_marker = 0;
        bool g_hand_debug_enabled = false;
        bool g_hand_ruler_enabled = false;
        bool g_hand_debug_left = true;
        bool g_hand_debug_rotation = false;
        bool g_key_down[9]{};
        ULONGLONG g_key_repeat_at[9]{};

        // Saved gear placement, body-frame metres. Pistols use the holster
        // zone centres; the others have their own keys.
        bool g_gear_tuning_enabled = false;
        bool g_gear_saved_valid[TuneGearCount]{};
        float g_gear_saved[TuneGearCount][3]{};
        bool g_light_centre_valid = false;
        float g_light_centre[3]{};

        // One tuning pass: four steps, each item attached to a controller
        // until that hand's trigger places it.
        struct GearSlot { int gear; int hand; };
        const int kGearStepCount = 4;
        const GearSlot kGearSteps[kGearStepCount][2] = {
            { { TuneGearLeftPistol, 0 }, { TuneGearRightPistol, 1 } },
            { { TuneGearBinoculars, 0 }, { TuneGearGrapple, 1 } },
            { { TuneGearLight, 0 }, { TuneGearMedipack, 1 } },
            { { TuneGearGrenade, 0 }, { -1, -1 } },
        };
        const char* kGearNames[TuneGearCount] = {
            "left pistol", "right pistol", "grapple", "binoculars",
            "personal light", "grenade/flare pouch", "medipack pouch"
        };
        const wchar_t* kGearKeys[TuneGearCount][3] = {
            { L"holster_left_x", L"holster_left_y", L"holster_left_z" },
            { L"holster_right_x", L"holster_right_y", L"holster_right_z" },
            { L"gear_grapple_x", L"gear_grapple_y", L"gear_grapple_z" },
            { L"gear_binoculars_x", L"gear_binoculars_y",
              L"gear_binoculars_z" },
            { L"gear_light_x", L"gear_light_y", L"gear_light_z" },
            { L"gear_grenade_x", L"gear_grenade_y", L"gear_grenade_z" },
            { L"gear_medipack_x", L"gear_medipack_y", L"gear_medipack_z" },
        };
        bool g_gear_tuning = false;
        bool g_gear_request = false;
        // Hand calibration corrections; reloaded with the hand offsets.
        bool g_hand_correction_loaded = false;
        bool g_hand_correction_valid[2] = { false, false };
        float g_hand_correction[2][12]{};
        const wchar_t* kHandCorrectionKeys[2] = {
            L"hand_calibration_left", L"hand_calibration_right" };
        bool g_gear_menu_request = false;
        int g_gear_step = 0;
        bool g_gear_placed[TuneGearCount]{};
        float g_gear_pending[TuneGearCount][3]{};

        const wchar_t* kPositionKeys[2][3] = {
            { L"hand_left_x", L"hand_left_y", L"hand_left_z" },
            { L"hand_right_x", L"hand_right_y", L"hand_right_z" }
        };
        const wchar_t* kRotationKeys[2][3] = {
            { L"hand_left_rot_x", L"hand_left_rot_y", L"hand_left_rot_z" },
            { L"hand_right_rot_x", L"hand_right_rot_y", L"hand_right_rot_z" }
        };
        const wchar_t* kCalibrationKeys[2][9] = {
            { L"hand_left_cal_m00", L"hand_left_cal_m01",
              L"hand_left_cal_m02", L"hand_left_cal_m10",
              L"hand_left_cal_m11", L"hand_left_cal_m12",
              L"hand_left_cal_m20", L"hand_left_cal_m21",
              L"hand_left_cal_m22" },
            { L"hand_right_cal_m00", L"hand_right_cal_m01",
              L"hand_right_cal_m02", L"hand_right_cal_m10",
              L"hand_right_cal_m11", L"hand_right_cal_m12",
              L"hand_right_cal_m20", L"hand_right_cal_m21",
              L"hand_right_cal_m22" }
        };

        void ini_path(wchar_t path[MAX_PATH])
        {
            swprintf_s(path, MAX_PATH, L"%strlvr.ini", exe_dir());
        }

        float g_marker_distance = -1.0f;

        void load_marker_distance()
        {
            wchar_t path[MAX_PATH]{};
            ini_path(path);
            wchar_t value[32]{};
            GetPrivateProfileStringW(L"vr", L"first_person_marker_distance",
                                     L"4.0", value, 32, path);
            const float d = (float)_wtof(value);
            g_marker_distance = std::isfinite(d) && d >= 0.3f && d <= 50.0f
                ? d : 4.0f;
        }

        bool tuning_key(int slot, int vk, bool repeat, ULONGLONG now)
        {
            const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (!down)
            {
                g_key_down[slot] = false;
                return false;
            }
            if (!g_key_down[slot])
            {
                g_key_down[slot] = true;
                g_key_repeat_at[slot] = now + 300;
                return true;
            }
            if (repeat && now >= g_key_repeat_at[slot])
            {
                g_key_repeat_at[slot] = now + 80;
                return true;
            }
            return false;
        }

        void save_grip_tuning()
        {
            wchar_t path[MAX_PATH]{};
            ini_path(path);
            bool ok = true;
            for (int axis = 0; axis < 3; ++axis)
            {
                wchar_t value[32]{};
                swprintf_s(value, L"%.4f", g_grip_pos[axis]);
                ok = WritePrivateProfileStringW(L"developer",
                    kGripPosKeys[axis], value, path) != 0 && ok;
                swprintf_s(value, L"%.1f", g_grip_rot[axis]);
                ok = WritePrivateProfileStringW(L"developer",
                    kGripRotKeys[axis], value, path) != 0 && ok;
            }
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
            log("tune: Numpad 7 %s the grip hand pose (pos %+.3f %+.3f %+.3f "
                "m, rot %+.1f %+.1f %+.1f deg)", ok ? "saved" : "FAILED to save",
                g_grip_pos[0], g_grip_pos[1], g_grip_pos[2],
                g_grip_rot[0], g_grip_rot[1], g_grip_rot[2]);
        }

        void save_hand_tuning()
        {
            if (config().hand_model == 2)
            {
                save_grip_tuning();
                return;
            }
            wchar_t path[MAX_PATH]{};
            ini_path(path);
            bool ok = true;
            for (int hand = 0; hand < 2; ++hand)
                for (int axis = 0; axis < 3; ++axis)
                {
                    wchar_t value[32]{};
                    swprintf_s(value, L"%.2f", g_hand_offset[hand][axis]);
                    ok = WritePrivateProfileStringW(L"developer",
                        kPositionKeys[hand][axis], value, path) != 0 && ok;
                    swprintf_s(value, L"%.2f", g_hand_rotation[hand][axis]);
                    ok = WritePrivateProfileStringW(L"developer",
                        kRotationKeys[hand][axis], value, path) != 0 && ok;
                }
            bool calibration_saved = false;
            bool calibration_ok = true;
            for (int hand = 0; hand < 2; ++hand)
            {
                float calibration[9]{};
                if (camera_first_person_hand_calibration(hand, calibration))
                {
                    calibration_saved = true;
                    calibration_ok = tune_save_hand_calibration(
                        hand == 0, calibration) && calibration_ok;
                }
            }
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
            log("tune: Numpad 7 %s hand alignment to trlvr.ini",
                ok ? "saved" : "FAILED to save");
            if (calibration_saved && !calibration_ok)
                log("tune: persistent hand calibration FAILED to save");
        }

        void nudge_hand(int axis, float direction)
        {
            if (config().hand_model == 2)
            {
                // One shared pose. Keys act in the SELECTED hand's frame:
                // the left hand is the mirror image, so x moves and y/z
                // rotations flip sign for it.
                const bool left = g_hand_debug_left;
                float sign = direction;
                if (left && ((!g_hand_debug_rotation && axis == 0) ||
                             (g_hand_debug_rotation && axis != 0)))
                    sign = -sign;
                float& value = g_hand_debug_rotation ? g_grip_rot[axis]
                                                     : g_grip_pos[axis];
                const float step = g_hand_debug_rotation ? 2.0f : 0.005f;
                const float limit = g_hand_debug_rotation ? 180.0f : 0.30f;
                value = fmaxf(-limit, fminf(limit, value + sign * step));
                log("tune: grip hand (via %s) %s %c = %.3f %s",
                    left ? "left" : "right",
                    g_hand_debug_rotation ? "rotation" : "position",
                    "xyz"[axis], value,
                    g_hand_debug_rotation ? "degrees" : "m");
                return;
            }
            const int hand = g_hand_debug_left ? 0 : 1;
            float& value = g_hand_debug_rotation
                ? g_hand_rotation[hand][axis] : g_hand_offset[hand][axis];
            const float limit = g_hand_debug_rotation ? 180.0f : 200.0f;
            const float step = 2.0f;
            value = fmaxf(-limit, fminf(limit, value + direction * step));
            log("tune: %s hand %s %c = %.1f %s",
                hand == 0 ? "left" : "right",
                g_hand_debug_rotation ? "rotation" : "position",
                "xyz"[axis], value,
                g_hand_debug_rotation ? "degrees" : "game units");
        }

        float read_zone_value(const wchar_t* path, const wchar_t* key,
                              float fallback, float min_value,
                              float max_value)
        {
            wchar_t text[64]{};
            GetPrivateProfileStringW(L"developer", key, L"", text,
                                     _countof(text), path);
            if (!text[0])
                return fallback;
            wchar_t* end = nullptr;
            const float value = wcstof(text, &end);
            return end != text && !*end && std::isfinite(value) &&
                   value >= min_value && value <= max_value
                ? value : fallback;
        }

        bool read_optional_value(const wchar_t* path, const wchar_t* key,
                                 float min_value, float max_value,
                                 float* out)
        {
            wchar_t text[64]{};
            GetPrivateProfileStringW(L"developer", key, L"", text,
                                     _countof(text), path);
            if (!text[0])
                return false;
            wchar_t* end = nullptr;
            const float value = wcstof(text, &end);
            if (end == text || *end || !std::isfinite(value) ||
                value < min_value || value > max_value)
                return false;
            *out = value;
            return true;
        }

        void load_hand_offsets();

        void save_gear_tuning()
        {
            wchar_t path[MAX_PATH]{};
            ini_path(path);
            bool ok = true;
            for (int gear = 0; gear < TuneGearCount; ++gear)
                for (int axis = 0; axis < 3; ++axis)
                {
                    wchar_t value[32]{};
                    swprintf_s(value, L"%.3f", g_gear_pending[gear][axis]);
                    ok = WritePrivateProfileStringW(L"developer",
                        kGearKeys[gear][axis], value, path) != 0 && ok;
                }
            ok = WritePrivateProfileStringW(L"developer",
                L"gear_custom_positions", L"1", path) != 0 && ok;
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
            log("tune: gear tuning %s all seven positions to trlvr.ini",
                ok ? "saved" : "FAILED to save");
            load_hand_offsets();
        }

        void log_gear_step()
        {
            if (g_gear_step == 0)
                log("tune: gear tuning step 1/4 -- pistols follow the "
                    "controllers; each trigger places its own holster");
            else if (g_gear_step == 1)
                log("tune: gear tuning step 2/4 -- left trigger places the "
                    "binoculars, right trigger places the grapple");
            else if (g_gear_step == 2)
                log("tune: gear tuning step 3/4 -- left trigger places the "
                    "personal light and its chest zone, right trigger the "
                    "medipack pouch");
            else
                log("tune: gear tuning step 4/4 -- left trigger places the "
                    "grenade/flare pouch");
        }

        bool aim_key(int slot, int vk, bool repeat, ULONGLONG now)
        {
            const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (!down)
            {
                g_aim_key_down[slot] = false;
                return false;
            }
            if (!g_aim_key_down[slot])
            {
                g_aim_key_down[slot] = true;
                g_aim_key_repeat_at[slot] = now + 300;
                return true;
            }
            if (repeat && now >= g_aim_key_repeat_at[slot])
            {
                g_aim_key_repeat_at[slot] = now + 60;
                return true;
            }
            return false;
        }

        void save_aim_tuning()
        {
            wchar_t path[MAX_PATH]{};
            ini_path(path);
            wchar_t value[32]{};
            const bool left = config().left_handed;
            swprintf_s(value, L"%.1f", left ? g_aim_pitch_left : g_aim_pitch);
            bool ok = WritePrivateProfileStringW(L"developer",
                left ? L"aim_pitch_left" : L"aim_pitch", value, path) != 0;
            swprintf_s(value, L"%.1f", left ? g_aim_yaw_left : g_aim_yaw);
            ok = WritePrivateProfileStringW(L"developer",
                left ? L"aim_yaw_left" : L"aim_yaw", value, path) != 0 && ok;
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
            log("tune: %s aim offset pitch %+.1f, yaw %+.1f degrees %s",
                left ? "left-hand" : "right-hand",
                left ? g_aim_pitch_left : g_aim_pitch,
                left ? g_aim_yaw_left : g_aim_yaw,
                ok ? "saved to trlvr.ini" : "FAILED to save");
        }

        // Returns true when aim tuning owns the numpad this frame.
        bool update_aim_tuning()
        {
            if (!g_aim_debug_enabled || !camera_first_person_active())
                return false;
            const ULONGLONG now = GetTickCount64();
            // Numpad 8/2 raise/lower, 4/6 left/right, half a degree a step.
            const int keys[4] = { VK_NUMPAD8, VK_NUMPAD2, VK_NUMPAD4,
                                  VK_NUMPAD6 };
            const float step = 0.5f;
            bool changed = false;
            if (config().hand_model == 2)
            {
                // Grip-model aim: one ray, mirrored for the left hand, so
                // the yaw key direction flips when left-handed.
                bool grip_changed = false;
                for (int i = 0; i < 4; ++i)
                {
                    if (!aim_key(i, keys[i], true, now) || !g_grip_aim_valid)
                        continue;
                    float sign = (i == 0 || i == 3) ? 1.0f : -1.0f;
                    if (i >= 2 && config().left_handed)
                        sign = -sign;
                    float& value = i < 2 ? g_grip_aim_pitch : g_grip_aim_yaw;
                    value = fmaxf(-180.0f, fminf(180.0f, value + sign * step));
                    grip_changed = true;
                }
                if (grip_changed)
                    log("tune: grip aim pitch %+.1f, yaw %+.1f degrees "
                        "(Numpad 7 saves)", g_grip_aim_pitch, g_grip_aim_yaw);
                if (aim_key(5, VK_NUMPAD7, false, now) && g_grip_aim_valid)
                    tune_set_grip_aim(g_grip_aim_pitch, g_grip_aim_yaw);
                return true;
            }
            for (int i = 0; i < 4; ++i)
            {
                if (!aim_key(i, keys[i], true, now))
                    continue;
                const bool left = config().left_handed;
                float& value = i < 2 ? (left ? g_aim_pitch_left : g_aim_pitch)
                                     : (left ? g_aim_yaw_left : g_aim_yaw);
                const float sign = (i == 0 || i == 3) ? 1.0f : -1.0f;
                value = fmaxf(-90.0f, fminf(90.0f, value + sign * step));
                changed = true;
            }
            if (aim_key(4, VK_NUMPAD5, false, now))
            {
                if (config().left_handed)
                    g_aim_pitch_left = g_aim_yaw_left = 0.0f;
                else
                    g_aim_pitch = g_aim_yaw = 0.0f;
                changed = true;
            }
            if (changed)
                log("tune: %s aim offset pitch %+.1f, yaw %+.1f degrees "
                    "(Numpad 7 saves)",
                    config().left_handed ? "left-hand" : "right-hand",
                    config().left_handed ? g_aim_pitch_left : g_aim_pitch,
                    config().left_handed ? g_aim_yaw_left : g_aim_yaw);
            if (aim_key(5, VK_NUMPAD7, false, now))
                save_aim_tuning();
            return true;
        }

        void load_hand_offsets()
        {
            wchar_t path[MAX_PATH]{};
            ini_path(path);
            g_hand_correction_loaded = false;
            for (int hand = 0; hand < 2; ++hand)
                for (int axis = 0; axis < 3; ++axis)
                {
                    wchar_t value[64]{};
                    GetPrivateProfileStringW(L"developer", kPositionKeys[hand][axis],
                                             L"0", value, _countof(value), path);
                    const float amount = static_cast<float>(_wtof(value));
                    g_hand_offset[hand][axis] =
                        std::isfinite(amount) && fabsf(amount) <= 200.0f
                            ? amount : 0.0f;
                    g_hand_rotation[hand][axis] = read_zone_value(
                        path, kRotationKeys[hand][axis], 0.0f,
                        -180.0f, 180.0f);
                }
            g_hand_offset_loaded = true;
            g_hand_debug_enabled = GetPrivateProfileIntW(L"developer",
                L"hand_tuning_debug", 0, path) != 0;
            g_hand_ruler_enabled = GetPrivateProfileIntW(L"developer",
                L"hand_ruler", 0, path) != 0;
            g_first_person_belt_height = read_zone_value(
                path, L"first_person_belt_height", 12.0f,
                -60.0f, 60.0f);
            g_ledge_grab_height = read_zone_value(
                path, L"ledge_grab_height", 0.12f,
                -0.20f, 0.35f);
            g_ledge_grab_debug_draw = GetPrivateProfileIntW(L"developer",
                L"ledge_grab_debug_draw", 0, path) != 0;
            g_grip_prompt_scale = read_zone_value(path, L"grip_prompt_scale",
                                                  0.25f, 0.02f, 2.0f);
            g_aim_pitch = read_zone_value(path, L"aim_pitch", 0.0f,
                                          -90.0f, 90.0f);
            g_aim_yaw = read_zone_value(path, L"aim_yaw", 0.0f,
                                        -90.0f, 90.0f);
            for (int axis = 0; axis < 3; ++axis)
            {
                g_grip_pos[axis] = read_zone_value(path, kGripPosKeys[axis],
                                                   0.0f, -0.30f, 0.30f);
                g_grip_rot[axis] = read_zone_value(path, kGripRotKeys[axis],
                                                   0.0f, -180.0f, 180.0f);
            }
            g_grip_cal_valid = true;
            for (int i = 0; i < 9; ++i)
            {
                float value = 0.0f;
                g_grip_cal_valid = g_grip_cal_valid &&
                    read_optional_value(path, kGripCalKeys[i], -1.25f, 1.25f,
                                        &value);
                g_grip_cal[i] = value;
            }
            {
                float pitch = 0.0f, yaw = 0.0f;
                g_grip_aim_valid =
                    read_optional_value(path, L"grip_aim_pitch", -180.0f,
                                        180.0f, &pitch) &&
                    read_optional_value(path, L"grip_aim_yaw", -180.0f,
                                        180.0f, &yaw);
                g_grip_aim_pitch = pitch;
                g_grip_aim_yaw = yaw;
            }
            g_aim_pitch_left = read_zone_value(path, L"aim_pitch_left",
                                               g_aim_pitch, -90.0f, 90.0f);
            g_aim_yaw_left = read_zone_value(path, L"aim_yaw_left",
                                             -g_aim_yaw, -90.0f, 90.0f);
            g_aim_debug_enabled = GetPrivateProfileIntW(L"developer",
                L"aim_tuning_debug", 0, path) != 0;
            const VrHolsterZone defaults = vr_default_holster_zone();
            g_holster_zone.left_x = read_zone_value(path, L"holster_left_x",
                defaults.left_x, -1.5f, 1.5f);
            g_holster_zone.right_x = read_zone_value(path, L"holster_right_x",
                defaults.right_x, -1.5f, 1.5f);
            g_holster_zone.y = read_zone_value(path, L"holster_y",
                defaults.y, -1.5f, 0.0f);
            g_holster_zone.z = read_zone_value(path, L"holster_z",
                defaults.z, -1.5f, 1.5f);
            g_holster_zone.radius_x = read_zone_value(path,
                L"holster_radius_x", defaults.radius_x, 0.05f, 0.8f);
            g_holster_zone.radius_y = read_zone_value(path,
                L"holster_radius_y", defaults.radius_y, 0.05f, 0.8f);
            g_holster_zone.radius_z = read_zone_value(path,
                L"holster_radius_z", defaults.radius_z, 0.05f, 0.8f);
            g_holster_zone.debug_draw = GetPrivateProfileIntW(L"developer",
                L"holster_debug_draw", defaults.debug_draw ? 1 : 0, path) != 0;
            // Per-side keys are written only by gear tuning; older INIs
            // keep the shared height and depth.
            g_holster_zone.left_y = read_zone_value(path, L"holster_left_y",
                g_holster_zone.y, -1.5f, 0.5f);
            g_holster_zone.left_z = read_zone_value(path, L"holster_left_z",
                g_holster_zone.z, -1.5f, 1.5f);
            g_holster_zone.right_y = read_zone_value(path, L"holster_right_y",
                g_holster_zone.y, -1.5f, 0.5f);
            g_holster_zone.right_z = read_zone_value(path, L"holster_right_z",
                g_holster_zone.z, -1.5f, 1.5f);

            g_gear_tuning_enabled = GetPrivateProfileIntW(L"developer",
                L"gear_tuning", 0, path) != 0;
            const bool custom = GetPrivateProfileIntW(L"developer",
                L"gear_custom_positions", 0, path) != 0;
            for (int gear = 0; gear < TuneGearCount; ++gear)
            {
                g_gear_saved_valid[gear] = false;
                if (!custom)
                    continue;
                if (gear == TuneGearLeftPistol || gear == TuneGearRightPistol)
                {
                    const bool left = gear == TuneGearLeftPistol;
                    g_gear_saved[gear][0] = left ? g_holster_zone.left_x
                                                 : g_holster_zone.right_x;
                    g_gear_saved[gear][1] = left ? g_holster_zone.left_y
                                                 : g_holster_zone.right_y;
                    g_gear_saved[gear][2] = left ? g_holster_zone.left_z
                                                 : g_holster_zone.right_z;
                    g_gear_saved_valid[gear] = true;
                    continue;
                }
                bool valid = true;
                for (int axis = 0; axis < 3; ++axis)
                    valid = read_optional_value(path, kGearKeys[gear][axis],
                        -1.5f, 1.5f, &g_gear_saved[gear][axis]) && valid;
                g_gear_saved_valid[gear] = valid;
            }
            g_light_centre_valid = g_gear_saved_valid[TuneGearLight];
            for (int axis = 0; axis < 3; ++axis)
                g_light_centre[axis] = g_gear_saved[TuneGearLight][axis];
            log("tune: headset-local hand offsets reloaded: "
                "left (%.1f,%.1f,%.1f), right (%.1f,%.1f,%.1f) game units",
                g_hand_offset[0][0], g_hand_offset[0][1],
                g_hand_offset[0][2], g_hand_offset[1][0],
                g_hand_offset[1][1], g_hand_offset[1][2]);
            log("tune: controller-local hand rotations reloaded: "
                "left (%.1f,%.1f,%.1f), right (%.1f,%.1f,%.1f) degrees XYZ",
                g_hand_rotation[0][0], g_hand_rotation[0][1],
                g_hand_rotation[0][2], g_hand_rotation[1][0],
                g_hand_rotation[1][1], g_hand_rotation[1][2]);
            log("tune: hand tuning debug %s; selected %s hand, %s mode",
                g_hand_debug_enabled ? "on" : "off",
                g_hand_debug_left ? "left" : "right",
                g_hand_debug_rotation ? "rotation" : "position");
            log("tune: player-facing belt visual height %+.1f game units",
                g_first_person_belt_height);
            log("tune: ledge grab target %+.2f m above animated wrist",
                g_ledge_grab_height);
            log("tune: ledge/vine grab-zone outlines %s",
                g_ledge_grab_debug_draw ? "on" : "off");
            log("tune: controller aim ray offset pitch %+.1f, yaw %+.1f "
                "degrees; aim tuning debug %s", g_aim_pitch, g_aim_yaw,
                g_aim_debug_enabled ? "on (numpad 8/2 pitch, 4/6 yaw, "
                "5 reset, 7 save)" : "off");
            log("tune: holster zones left (%.2f,%.2f,%.2f), right "
                "(%.2f,%.2f,%.2f); radius %.2f/%.2f/%.2f, debug %d",
                g_holster_zone.left_x, g_holster_zone.left_y,
                g_holster_zone.left_z, g_holster_zone.right_x,
                g_holster_zone.right_y, g_holster_zone.right_z,
                g_holster_zone.radius_x,
                g_holster_zone.radius_y, g_holster_zone.radius_z,
                g_holster_zone.debug_draw ? 1 : 0);
            log("tune: gear tuning %s; custom gear positions %s "
                "(binoculars %d, grapple %d, light %d)",
                g_gear_tuning_enabled ? "available (double-click right "
                "stick or F6)" : "off",
                custom ? "on" : "off",
                g_gear_saved_valid[TuneGearBinoculars] ? 1 : 0,
                g_gear_saved_valid[TuneGearGrapple] ? 1 : 0,
                g_gear_saved_valid[TuneGearLight] ? 1 : 0);
        }
    }

    float tune_ipd_scale()          { return 1.0f; }
    float tune_marker_distance()
    {
        if (g_marker_distance < 0.0f)
            load_marker_distance();
        return g_marker_distance;
    }
    // Board mode scales the whole VR side (stereo, head movement, hands,
    // gear) by the tabletop factor while its camera is in use.
    float tune_world_scale()
    {
        return config().world_scale * camera_world_scale_factor();
    }
    float tune_look_speed()         { return config().look_speed; }
    bool  tune_swap_eyes()          { return false; }
    float tune_cull_fov()           { return config().cull_fov_degrees; }
    bool  tune_level_horizon()      { return config().level_horizon; }
    bool  tune_camera_auto_center() { return config().camera_auto_center; }
    bool  tune_camera_shake()       { return config().camera_shake; }
    float tune_move_scale()         { return config().head_position_scale; }
    float tune_turn_scale()         { return config().head_rotation_scale; }
    float tune_ui_scale()           { return config().ui_scale; }
    float tune_hud_follow()         { return config().hud_follow; }

    void tune_hand_offset(bool left, float out[3])
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        const int hand = left ? 0 : 1;
        for (int axis = 0; axis < 3; ++axis)
            out[axis] = g_hand_offset[hand][axis];
    }

    void tune_hand_rotation(bool left, float out[3])
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        const int hand = left ? 0 : 1;
        for (int axis = 0; axis < 3; ++axis)
            out[axis] = g_hand_rotation[hand][axis];
    }

    void tune_aim_offset_hand(bool left, float* pitch_degrees,
                              float* yaw_degrees)
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        if (pitch_degrees)
            *pitch_degrees = left ? g_aim_pitch_left : g_aim_pitch;
        if (yaw_degrees)
            *yaw_degrees = left ? g_aim_yaw_left : g_aim_yaw;
    }

    void tune_grip_hand(float position_metres[3], float rotation_degrees[3])
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        for (int axis = 0; axis < 3; ++axis)
        {
            if (position_metres)
                position_metres[axis] = g_grip_pos[axis];
            if (rotation_degrees)
                rotation_degrees[axis] = g_grip_rot[axis];
        }
    }

    bool tune_grip_hand_calibration(float out[9])
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        if (!g_grip_cal_valid || !out)
            return false;
        for (int i = 0; i < 9; ++i)
            out[i] = g_grip_cal[i];
        return true;
    }

    bool tune_set_grip_hand_calibration(const float in[9])
    {
        if (!in)
            return false;
        wchar_t path[MAX_PATH]{};
        ini_path(path);
        bool ok = true;
        for (int i = 0; i < 9; ++i)
        {
            if (!std::isfinite(in[i]) || fabsf(in[i]) > 1.25f)
                return false;
            g_grip_cal[i] = in[i];
            wchar_t value[32]{};
            swprintf_s(value, L"%.9f", in[i]);
            ok = WritePrivateProfileStringW(L"developer", kGripCalKeys[i],
                                            value, path) != 0 && ok;
        }
        g_grip_cal_valid = true;
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
        return ok;
    }

    bool tune_grip_aim(float* pitch_degrees, float* yaw_degrees)
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        if (!g_grip_aim_valid)
            return false;
        if (pitch_degrees)
            *pitch_degrees = g_grip_aim_pitch;
        if (yaw_degrees)
            *yaw_degrees = g_grip_aim_yaw;
        return true;
    }

    bool tune_set_grip_aim(float pitch_degrees, float yaw_degrees)
    {
        if (!std::isfinite(pitch_degrees) || !std::isfinite(yaw_degrees))
            return false;
        g_grip_aim_pitch = pitch_degrees;
        g_grip_aim_yaw = yaw_degrees;
        g_grip_aim_valid = true;
        wchar_t path[MAX_PATH]{};
        ini_path(path);
        wchar_t value[32]{};
        swprintf_s(value, L"%.1f", pitch_degrees);
        bool ok = WritePrivateProfileStringW(L"developer", L"grip_aim_pitch",
                                             value, path) != 0;
        swprintf_s(value, L"%.1f", yaw_degrees);
        ok = WritePrivateProfileStringW(L"developer", L"grip_aim_yaw", value,
                                        path) != 0 && ok;
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
        log("tune: grip aim pitch %+.1f, yaw %+.1f degrees %s", pitch_degrees,
            yaw_degrees, ok ? "saved to trlvr.ini" : "FAILED to save");
        return ok;
    }

    bool tune_hand_correction(bool left, float out[12])
    {
        const int hand = left ? 0 : 1;
        if (!g_hand_correction_loaded)
        {
            g_hand_correction_loaded = true;
            wchar_t path[MAX_PATH]{};
            ini_path(path);
            for (int h = 0; h < 2; ++h)
            {
                g_hand_correction_valid[h] = false;
                wchar_t text[512]{};
                GetPrivateProfileStringW(L"vr", kHandCorrectionKeys[h], L"",
                                         text, _countof(text), path);
                float values[12]{};
                const wchar_t* p = text;
                int n = 0;
                for (; n < 12; ++n)
                {
                    wchar_t* end = nullptr;
                    values[n] = wcstof(p, &end);
                    if (end == p || !std::isfinite(values[n]))
                        break;
                    p = end;
                }
                // Rotation entries are at most 1; the translation is a
                // correction of centimetres, never more than half a metre.
                bool ok = n == 12;
                for (int i = 0; ok && i < 12; ++i)
                    ok = fabsf(values[i]) <= (i < 9 ? 1.01f : 0.5f);
                if (ok)
                {
                    for (int i = 0; i < 12; ++i)
                        g_hand_correction[h][i] = values[i];
                    g_hand_correction_valid[h] = true;
                }
                else if (text[0])
                    log("tune: [vr] %S ignored (needs 12 numbers: a "
                        "rotation and a translation of at most 0.5 m)",
                        kHandCorrectionKeys[h]);
            }
            log("tune: hand calibration left %s, right %s",
                g_hand_correction_valid[0] ? "saved" : "default",
                g_hand_correction_valid[1] ? "saved" : "default");
        }
        if (!g_hand_correction_valid[hand])
            return false;
        if (out)
            for (int i = 0; i < 12; ++i)
                out[i] = g_hand_correction[hand][i];
        return true;
    }

    bool tune_set_hand_correction(bool left, const float in[12])
    {
        const int hand = left ? 0 : 1;
        tune_hand_correction(left, nullptr);
        wchar_t path[MAX_PATH]{};
        ini_path(path);
        wchar_t text[512]{};
        if (in)
        {
            for (int i = 0; i < 12; ++i)
                if (!std::isfinite(in[i]) ||
                    fabsf(in[i]) > (i < 9 ? 1.01f : 0.5f))
                    return false;
            size_t used = 0;
            for (int i = 0; i < 12; ++i)
                used += swprintf_s(text + used, _countof(text) - used,
                                   i ? L" %.6f" : L"%.6f", in[i]);
            for (int i = 0; i < 12; ++i)
                g_hand_correction[hand][i] = in[i];
        }
        g_hand_correction_valid[hand] = in != nullptr;
        const bool ok = WritePrivateProfileStringW(L"vr",
            kHandCorrectionKeys[hand], in ? text : nullptr, path) != 0;
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
        return ok;
    }

    bool tune_load_hand_calibration(bool left, float out[9])
    {
        if (!out)
            return false;
        wchar_t path[MAX_PATH]{};
        ini_path(path);
        const int hand = left ? 0 : 1;
        float values[9]{};
        for (int i = 0; i < 9; ++i)
        {
            wchar_t text[64]{};
            GetPrivateProfileStringW(L"developer", kCalibrationKeys[hand][i],
                                     L"", text, _countof(text), path);
            if (!text[0])
                return false;
            wchar_t* end = nullptr;
            const float value = wcstof(text, &end);
            if (end == text || *end || !std::isfinite(value) ||
                fabsf(value) > 1.25f)
                return false;
            values[i] = value;
        }
        for (int i = 0; i < 9; ++i)
            out[i] = values[i];
        return true;
    }

    bool tune_save_hand_calibration(bool left, const float in[9])
    {
        if (!in)
            return false;
        wchar_t path[MAX_PATH]{};
        ini_path(path);
        const int hand = left ? 0 : 1;
        bool ok = true;
        for (int i = 0; i < 9; ++i)
        {
            if (!std::isfinite(in[i]) || fabsf(in[i]) > 1.25f)
                return false;
            wchar_t value[32]{};
            swprintf_s(value, L"%.9f", in[i]);
            ok = WritePrivateProfileStringW(L"developer",
                kCalibrationKeys[hand][i], value, path) != 0 && ok;
        }
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
        return ok;
    }

    bool tune_hand_ruler_enabled()
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        return g_hand_ruler_enabled;
    }

    bool tune_hand_debug_enabled()
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        return g_hand_debug_enabled;
    }

    bool tune_hand_debug_selected_left() { return g_hand_debug_left; }
    bool tune_hand_debug_rotation_mode() { return g_hand_debug_rotation; }

    float tune_first_person_belt_height()
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        return g_first_person_belt_height;
    }

    float tune_ledge_grab_height()
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        return g_ledge_grab_height;
    }

    bool tune_view_prescale_fix()
    {
        if (g_view_prescale_fix < 0)
            g_view_prescale_fix = config().view_prescale_fix ? 1 : 0;
        return g_view_prescale_fix != 0;
    }

    bool tune_aim_debug_enabled()
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        return g_aim_debug_enabled;
    }

    void tune_aim_offset(float* pitch_degrees, float* yaw_degrees)
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        const bool left = config().left_handed;
        if (pitch_degrees)
            *pitch_degrees = left ? g_aim_pitch_left : g_aim_pitch;
        if (yaw_degrees)
            *yaw_degrees = left ? g_aim_yaw_left : g_aim_yaw;
    }

    float tune_grip_prompt_scale()
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        return g_grip_prompt_scale;
    }

    bool tune_ledge_grab_debug_draw()
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        return g_ledge_grab_debug_draw;
    }

    const VrHolsterZone& tune_holster_zone()
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        return g_holster_zone;
    }

    void tune_update()
    {
        HWND foreground = GetForegroundWindow();
        if (!foreground)
            return;
        DWORD pid = 0;
        GetWindowThreadProcessId(foreground, &pid);
        if (pid != GetCurrentProcessId())
            return;
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        if (GetAsyncKeyState(VK_F1) & 1)
            vr_recenter();
        if (GetAsyncKeyState(VK_F5) & 1)
            load_hand_offsets();
        if (GetAsyncKeyState(VK_F7) & 1)
        {
            g_view_prescale_fix = tune_view_prescale_fix() ? 0 : 1;
            log("tune: F7 view prescale fix %s (true VR proportions %s)",
                g_view_prescale_fix ? "ON" : "OFF",
                g_view_prescale_fix ? "restored" : "disabled for comparison");
        }
        if ((GetAsyncKeyState(VK_F6) & 1) && g_gear_tuning_enabled)
            g_gear_request = true;
        if (GetAsyncKeyState(VK_F9) & 1)
            hud_capture_start();
        // F2: first-person object culling open / headset frustum (cull.h).
        if (GetAsyncKeyState(VK_F2) & 1)
        {
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
                cull_toggle_terrain_culling();
            else
                cull_toggle_object_culling();
        }
        // F3: GPU time of each part of the next frame (gpu_profile.h).
        if (GetAsyncKeyState(VK_F3) & 1)
            gpu_profile_request();
        if (GetAsyncKeyState(VK_F4) & 1)
        {
            hud_scene_capture_start();
            vr_world_capture_start();
        }
        // [ and ]: first-person world-marker convergence distance
        // (user, 2026-10-04: the caution icon's depth needs tuning).
        {
            const bool nearer = (GetAsyncKeyState(VK_OEM_4) & 1) != 0;
            const bool farther = (GetAsyncKeyState(VK_OEM_6) & 1) != 0;
            if ((nearer || farther) && camera_first_person_active())
            {
                float d = tune_marker_distance() * (farther ? 1.15f
                                                            : 1.0f / 1.15f);
                d = d < 0.3f ? 0.3f : d > 50.0f ? 50.0f : d;
                g_marker_distance = d;
                wchar_t path[MAX_PATH]{};
                ini_path(path);
                wchar_t value[32]{};
                swprintf_s(value, L"%.2f", d);
                const bool ok = WritePrivateProfileStringW(L"vr",
                    L"first_person_marker_distance", value, path) != 0;
                log("tune: first-person marker distance %.2f m (%s)", d,
                    ok ? "saved" : "save FAILED");
            }
        }
        // Pose debug ([developer] board_pose_debug = 1): [ ] \ fingers,
        // ; ' dangle animation.
        {
            static int pose_debug = -1;
            if (pose_debug < 0)
            {
                wchar_t path[MAX_PATH]{};
                swprintf_s(path, L"%strlvr.ini", exe_dir());
                pose_debug = (int)GetPrivateProfileIntW(L"developer",
                    L"board_pose_debug", 0, path) != 0 ? 1 : 0;
                if (pose_debug)
                    log("tune: pose debug keys on (third person) -- numpad "
                        "4/6, 8/2, 9/3 move where Lara hangs from the pinch "
                        "along the hand's x, y, z (5 mm); 5 resets");
            }
            if (pose_debug && !camera_first_person_active())
            {
                // Numpad 4/6 x, 2/8 y, 3/9 z, 5 reset.
                const int keys[7] = { VK_NUMPAD4, VK_NUMPAD6, VK_NUMPAD2,
                                      VK_NUMPAD8, VK_NUMPAD3, VK_NUMPAD9,
                                      VK_NUMPAD5 };
                for (int k = 0; k < 7; ++k)
                    if (GetAsyncKeyState(keys[k]) & 1)
                        camera_pose_debug_key(10 + k);
                // ; and ' cycle the flick knockdown animation.
                if (GetAsyncKeyState(VK_OEM_1) & 1)
                    camera_pose_debug_key(21);
                if (GetAsyncKeyState(VK_OEM_7) & 1)
                    camera_pose_debug_key(20);
            }
        }
        if (GetAsyncKeyState(VK_F8) & 1)
        {
            ++g_ledge_visibility_marker;
            log("tune: ledge hand geometry/projection sample %u (F8)",
                g_ledge_visibility_marker);
        }
        if (update_aim_tuning())
            return;
        if (!g_hand_debug_enabled || !config().first_person_tracked_hands ||
            !camera_first_person_active())
            return;
        // The numpad tunes the foregrip while a long gun is held two-handed.
        if (camera_two_hand_engaged())
            return;

        const ULONGLONG now = GetTickCount64();
        if (tuning_key(0, VK_MULTIPLY, false, now))
        {
            g_hand_debug_left = !g_hand_debug_left;
            log("tune: selected %s hand",
                g_hand_debug_left ? "left" : "right");
        }
        if (tuning_key(1, VK_NUMPAD1, false, now))
        {
            g_hand_debug_rotation = !g_hand_debug_rotation;
            log("tune: %s mode",
                g_hand_debug_rotation ? "rotation" : "position");
        }
        if (tuning_key(2, VK_NUMPAD7, false, now))
            save_hand_tuning();

        // Position: left/right, forward/back, up/down. Engine-local Y points
        // down and Z forward. Rotation: yaw, pitch, roll on the same key pairs.
        const int keys[6] = { VK_NUMPAD4, VK_NUMPAD6, VK_NUMPAD8,
                              VK_NUMPAD2, VK_NUMPAD9, VK_NUMPAD3 };
        const int axes[6] = { g_hand_debug_rotation ? 1 : 0,
                              g_hand_debug_rotation ? 1 : 0,
                              g_hand_debug_rotation ? 0 : 2,
                              g_hand_debug_rotation ? 0 : 2,
                              g_hand_debug_rotation ? 2 : 1,
                              g_hand_debug_rotation ? 2 : 1 };
        const float signs[6] = { -1, +1, +1, -1, -1, +1 };
        for (int i = 0; i < 6; ++i)
            if (tuning_key(i + 3, keys[i], true, now))
                nudge_hand(axes[i], signs[i]);
    }

    unsigned tune_ledge_visibility_marker()
    {
        return g_ledge_visibility_marker;
    }

    bool tune_gear_tuning_enabled()
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        return g_gear_tuning_enabled;
    }

    bool tune_gear_tuning_active() { return g_gear_tuning; }
    int tune_gear_tuning_step() { return g_gear_step; }
    int tune_gear_tuning_step_count() { return kGearStepCount; }

    int tune_gear_tuning_slot(int hand)
    {
        if (!g_gear_tuning || g_gear_step < 0 ||
            g_gear_step >= kGearStepCount)
            return -1;
        for (const GearSlot& slot : kGearSteps[g_gear_step])
            if (slot.hand == hand)
                return slot.gear;
        return -1;
    }

    bool tune_gear_tuning_placed(int gear)
    {
        return gear >= 0 && gear < TuneGearCount && g_gear_placed[gear];
    }

    const char* tune_gear_name(int gear)
    {
        return gear >= 0 && gear < TuneGearCount ? kGearNames[gear] : "";
    }

    void tune_gear_tuning_request_from_menu()
    {
        g_gear_menu_request = true;
        log("tune: holster setup requested from the pause menu");
    }

    bool tune_gear_tuning_menu_pending() { return g_gear_menu_request; }

    void tune_gear_tuning_clear_menu_request()
    {
        g_gear_menu_request = false;
    }

    bool tune_gear_tuning_take_request()
    {
        const bool request = g_gear_request;
        g_gear_request = false;
        return request;
    }

    void tune_gear_tuning_begin()
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        g_gear_tuning = true;
        g_gear_step = 0;
        for (bool& placed : g_gear_placed)
            placed = false;
        log("tune: gear tuning started; other controls are disabled. "
            "Double-click the right stick (or F6) to cancel.");
        log_gear_step();
    }

    void tune_gear_tuning_cancel(const char* reason)
    {
        if (!g_gear_tuning)
            return;
        g_gear_tuning = false;
        log("tune: gear tuning cancelled (%s); nothing saved",
            reason ? reason : "?");
    }

    int tune_gear_tuning_hand(int gear)
    {
        if (!g_gear_tuning || gear < 0 || gear >= TuneGearCount ||
            g_gear_placed[gear])
            return -1;
        for (const GearSlot& slot : kGearSteps[g_gear_step])
            if (slot.gear == gear)
                return slot.hand;
        return -1;
    }

    int tune_gear_tuning_trigger(bool left, const float body[3])
    {
        if (!g_gear_tuning || !body)
            return 0;
        const int hand = left ? 0 : 1;
        int gear = -1;
        for (const GearSlot& slot : kGearSteps[g_gear_step])
            if (slot.hand == hand)
                gear = slot.gear;
        if (gear < 0)
            return 0;
        if (g_gear_placed[gear])
        {
            g_gear_placed[gear] = false;
            log("tune: %s picked back up by the %s controller",
                kGearNames[gear], left ? "left" : "right");
            return -1;
        }
        for (int axis = 0; axis < 3; ++axis)
            g_gear_pending[gear][axis] = body[axis];
        g_gear_placed[gear] = true;
        log("tune: %s placed at right %+.3f, up %+.3f, forward %+.3f m",
            kGearNames[gear], body[0], body[1], body[2]);

        for (const GearSlot& slot : kGearSteps[g_gear_step])
            if (slot.gear >= 0 && !g_gear_placed[slot.gear])
                return 1;
        if (++g_gear_step < kGearStepCount)
        {
            log_gear_step();
            return 1;
        }
        g_gear_tuning = false;
        save_gear_tuning();
        log("tune: gear tuning finished; normal controls restored");
        return 1;
    }

    bool tune_gear_position(int gear, float body[3])
    {
        if (!body || gear < 0 || gear >= TuneGearCount)
            return false;
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        const float* source = nullptr;
        if (g_gear_tuning && g_gear_placed[gear])
            source = g_gear_pending[gear];
        else if (g_gear_saved_valid[gear])
            source = g_gear_saved[gear];
        if (!source)
            return false;
        for (int axis = 0; axis < 3; ++axis)
            body[axis] = source[axis];
        return true;
    }

    const float* tune_light_zone_centre()
    {
        if (!g_hand_offset_loaded)
            load_hand_offsets();
        return g_light_centre_valid ? g_light_centre : nullptr;
    }
}
