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
        bool g_want_previous_outfit = false;
        const uintptr_t kOutfitListHolder = 0x010C0A58;     // -> { ... +0xC: int16* list }
        const uintptr_t kObjectTable = 0x010EFC90;          // -> { int count; { int, char* name }[] }
        const uintptr_t kAltPlayerObjectId = kGameTracker + 0x438;

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
    void tr7_previous_outfit() { g_want_previous_outfit = true; }

    namespace
    {
    // The id of the outfit she wears now (instance +0x94 -> object +8), or 0.
    int current_outfit_id()
    {
        __try
        {
            const uintptr_t player =
                *reinterpret_cast<uintptr_t*>(kGameTracker + kTrackerPlayer);
            if (!player)
                return 0;
            const uintptr_t object = *reinterpret_cast<uintptr_t*>(player + 0x94);
            if (!object)
                return 0;
            return *reinterpret_cast<int*>(object + 8);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }
    }

    bool tr7_current_outfit_name(char* out, int size)
    {
        if (!out || size < 2)
            return false;
        out[0] = '\0';
        const int id = current_outfit_id();
        if (id <= 0)
            return false;
        __try
        {
            const uintptr_t table = *reinterpret_cast<uintptr_t*>(kObjectTable);
            if (!table || id > *reinterpret_cast<int*>(table))
                return false;
            const char* name = *reinterpret_cast<const char* const*>(
                table + 4 + (id - 1) * 8 + 4);
            if (!name)
                return false;
            int i = 0;
            for (; i < size - 1 && name[i]; ++i)
                out[i] = name[i];
            out[i] = '\0';
            return i > 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out[0] = '\0';
            return false;
        }
    }

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

    namespace
    {
    // The game's own next-outfit list (int16 count, then ids, per level) read
    // backwards: the id before the current one, wrapping to the last. The
    // switch takes any id through altPlayerObjectID (decoded from 0x5A40B0).
    void do_previous_outfit()
    {
        __try
        {
            const uintptr_t holder =
                *reinterpret_cast<uintptr_t*>(kOutfitListHolder);
            if (!holder)
                return;
            const short* list = *reinterpret_cast<const short* const*>(holder + 0xC);
            if (!list)
                return;
            const int count = list[0];
            if (count <= 0 || count > 256)
                return;
            const int current = current_outfit_id();
            int index = count;                  // not in the list: wrap to the last
            for (int i = 0; i < count; ++i)
                if (list[1 + i] == current)
                {
                    index = i;
                    break;
                }
            index = index <= 0 ? count - 1 : index - 1;
            const int target = list[1 + index];
            if (target <= 0)
                return;
            *reinterpret_cast<int*>(kAltPlayerObjectId) = target;
            reinterpret_cast<void(__cdecl*)()>(kSwitchPlayerCharacter)();
            log("tr7: switched to the previous outfit (id %d)", target);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            log("tr7: switching to the previous outfit faulted");
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
        if (g_want_previous_outfit)
        {
            g_want_previous_outfit = false;
            do_previous_outfit();
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
