#include "tr7_menu_hook.h"

#include "../common/log.h"

#include <windows.h>

namespace trlvr
{
    namespace
    {
        const uintptr_t kGameTracker = 0x010E5370;
        const uintptr_t kTrackerPlayer = 0x0C;          // Instance*
        const uintptr_t kTrackerTimeDilation = 0x13C;   // float
        const uintptr_t kInstanceData = 0x1C0;          // PlayerProp*
        const uintptr_t kPropHealthInitial = 0xA4;      // float
        // IncrHealth(float): adds to PlayerData+0x5A0 (disassembled).
        const uintptr_t kIncrHealth = 0x00576CA0;
        // PLAYER_DebugSwitchPlayerCharacter (our remap agrees).
        const uintptr_t kSwitchPlayerCharacter = 0x005A40B0;
        const uintptr_t kFlags[Tr7FlagCount] = {
            0x01075BD4,     // wireframe
            0x00CDC9A0,     // hide UI
            0x0107696C,     // debug keypad
        };
        const char* const kKey = "SOFTWARE\\TRAE-Menu-Hook";
        bool g_want_health = false;
        bool g_want_outfit = false;

        bool read(const char* name, void* out, DWORD size)
        {
            HKEY key = nullptr;
            if (RegOpenKeyExA(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &key) !=
                ERROR_SUCCESS)
                return false;
            DWORD got = size;
            const bool ok = RegQueryValueExA(key, name, nullptr, nullptr,
                                             (LPBYTE)out, &got) == ERROR_SUCCESS &&
                            got == size;
            RegCloseKey(key);
            return ok;
        }

        // As TR7-Menu-Hook saves them: REG_BINARY of the value's bytes.
        void write(const char* name, const void* value, DWORD size)
        {
            HKEY key = nullptr;
            if (RegCreateKeyExA(HKEY_CURRENT_USER, kKey, 0, nullptr, 0,
                                KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
                return;
            RegSetValueExA(key, name, 0, REG_BINARY, (const BYTE*)value, size);
            RegCloseKey(key);
            log("tr7: saved %s for TR7-Menu-Hook (applies when the game next "
                "starts)", name);
        }
    }

    bool tr7_menu_hook_present()
    {
        return GetModuleHandleW(L"TR7-Menu-Hook.asi") != nullptr;
    }

    void tr7_refill_health() { g_want_health = true; }
    void tr7_next_outfit() { g_want_outfit = true; }

    namespace
    {
    void do_refill_health()
    {
        __try
        {
            const uintptr_t player =
                *reinterpret_cast<uintptr_t*>(kGameTracker + kTrackerPlayer);
            if (!player)
                return;
            const uintptr_t prop =
                *reinterpret_cast<uintptr_t*>(player + kInstanceData);
            if (!prop)
                return;
            const float full =
                *reinterpret_cast<float*>(prop + kPropHealthInitial);
            reinterpret_cast<void(__cdecl*)(float)>(kIncrHealth)(full);
            log("tr7: health refilled (+%.0f)", full);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            log("tr7: refilling health faulted");
        }
    }

    void do_next_outfit()
    {
        __try
        {
            reinterpret_cast<void(__cdecl*)()>(kSwitchPlayerCharacter)();
            log("tr7: switched to the next outfit");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            log("tr7: switching outfit faulted");
        }
    }

    }

    void tr7_menu_hook_frame()
    {
        // Cleared first: a Present the action itself causes comes back here.
        if (g_want_health)
        {
            g_want_health = false;
            do_refill_health();
        }
        if (g_want_outfit)
        {
            g_want_outfit = false;
            do_next_outfit();
        }
    }

    float* tr7_time_multiplier()
    {
        return reinterpret_cast<float*>(kGameTracker + kTrackerTimeDilation);
    }

    bool tr7_flag(Tr7Flag flag, bool* value)
    {
        if (flag < 0 || flag >= Tr7FlagCount || !value)
            return false;
        __try
        {
            *value = *reinterpret_cast<const bool*>(kFlags[flag]);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void tr7_set_flag(Tr7Flag flag, bool value)
    {
        if (flag < 0 || flag >= Tr7FlagCount)
            return;
        __try
        {
            *reinterpret_cast<bool*>(kFlags[flag]) = value;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    int tr7_option_int(const char* name, int fallback)
    {
        int v = fallback;
        return read(name, &v, sizeof(v)) ? v : fallback;
    }
    void tr7_set_option_int(const char* name, int value)
    { write(name, &value, sizeof(value)); }

    bool tr7_option_bool(const char* name, bool fallback)
    {
        bool v = fallback;
        return read(name, &v, sizeof(v)) ? v : fallback;
    }
    void tr7_set_option_bool(const char* name, bool value)
    { write(name, &value, sizeof(value)); }

    float tr7_option_float(const char* name, float fallback)
    {
        float v = fallback;
        return read(name, &v, sizeof(v)) ? v : fallback;
    }
    void tr7_set_option_float(const char* name, float value)
    { write(name, &value, sizeof(value)); }
}
