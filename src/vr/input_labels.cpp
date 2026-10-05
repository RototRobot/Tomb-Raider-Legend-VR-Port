#include "input_labels.h"

#include "camera_head.h"
#include "hook.h"
#include "tune.h"
#include "ui_space.h"
#include "vr_input.h"

#include "../common/config.h"
#include "../common/log.h"

#include <cstring>

namespace trlvr
{
    namespace
    {
        // InputRemapDevice::GetButtonName(int) in the retail Steam exe. Hint
        // translation calls this for the coloured action token, so replacing
        // the returned name changes the token without touching ordinary text.
        const uintptr_t kGetButtonName = 0x004E1E00;

        struct GameUtf8String
        {
            int            characters;
            unsigned       bytes;
            unsigned       capacity;
            unsigned char* data;
        };
        static_assert(sizeof(GameUtf8String) == 16,
                      "retail cdc::UTF8String layout changed");

        typedef GameUtf8String* (__thiscall* GetButtonNameFn)(void*, int);
        typedef GameUtf8String* (__thiscall* StringCtorFn)(GameUtf8String*,
                                                           const char*);

        GetButtonNameFn g_original = nullptr;
        const StringCtorFn game_string_ctor =
            (StringCtorFn)0x005DCB70;

        struct Label
        {
            const char* text;
            GameUtf8String value;
            bool ready;
        };

        Label g_labels[] = {
            { "LT", {}, false },
            { "RT", {}, false },
            { "Left Grip", {}, false },
            { "Right Grip", {}, false },
            { "Right A", {}, false },
            { "Right B", {}, false },
            { "Left X", {}, false },
            { "Left Y", {}, false },
            { "Left A", {}, false },
            { "Left B", {}, false },
            { "Left Pad", {}, false },
            { "Right Pad", {}, false },
            { "Left Menu", {}, false },
            { "Right Menu", {}, false },
            { "Left Stick", {}, false },
            { "Right Stick", {}, false },
            { "Left Stick Hold", {}, false },
            { "Right Stick Hold", {}, false },
            { "Left Y Hold", {}, false },
            { "Left B Hold", {}, false },
            { "Right Menu Hold", {}, false },
            { "Chest + Left Grip", {}, false },
            { "Belt Grapple", {}, false },
            { "Belt Binoculars", {}, false },
            { "Hip Grip", {}, false },
            { "Auto Target", {}, false },
            { "Gear Cross", {}, false },
            { "Grip over your Shoulder", {}, false },
        };

        GameUtf8String* label(const char* text)
        {
            for (Label& item : g_labels)
            {
                if (strcmp(item.text, text) != 0)
                    continue;
                if (!item.ready)
                {
                    game_string_ctor(&item.value, item.text);
                    item.ready = true;
                }
                return &item.value;
            }
            return nullptr;
        }

        bool same(const char* value, const char* wanted)
        {
            return value && _stricmp(value, wanted) == 0;
        }

        bool any(const char* value, const char* a, const char* b = nullptr,
                 const char* c = nullptr, const char* d = nullptr)
        {
            return same(value, a) || (b && same(value, b)) ||
                   (c && same(value, c)) || (d && same(value, d));
        }

        // Labels follow what vr_input actually does (reviewed 2026-10-04):
        //  - left_handed swaps the triggers' roles in every mode (Action E
        //    on RT, fire on LT); the grips and buttons stay put;
        //  - first-person immersive drops Q, G, Z, Delete and Page Down
        //    for gestures: grapple and binoculars from the belt, guns drawn
        //    at the hips (precision aim is then the Action trigger),
        //    targeting automatic, light on the chest with the left grip;
        //  - third-person immersive moves weapon switch, medipack,
        //    binoculars and light onto the 3D gear cross.
        const char* replacement(const char* key, VrControllerStyle style)
        {
            const bool lefty = config().left_handed;
            const char* action_trigger = lefty ? "RT" : "LT";
            const char* fire_trigger = lefty ? "LT" : "RT";
            const bool immersive = config().immersive_controls;
            const bool first = immersive && camera_first_person_active();
            const bool third = immersive && !first;

            if (same(key, "E"))
                return action_trigger;
            if (any(key, "Mouse 1", "Mouse Button 1",
                    "Left Mouse Button", "Left Button"))
                return fire_trigger;
            if (same(key, "Q"))
                return first ? "Belt Grapple" : "Left Grip";
            if (same(key, "G"))
                return first ? "Auto Target" : "Right Grip";
            if (any(key, "Escape", "Esc"))
                return "Left Stick";
            if (same(key, "End"))
                return third ? "Gear Cross"
                     : first ? "Grip over your Shoulder" : "Right Stick";
            if (same(key, "Home"))
                return third ? "Gear Cross"
                     : style == VrControllerIndex ? "Left Pad"
                                                  : "Left Stick Hold";
            if (any(key, "Page Down", "PageDown", "PgDn", "Pg Dn"))
                return first ? "Belt Binoculars"
                     : third ? "Gear Cross"
                     : style == VrControllerIndex ? "Right Pad"
                                                  : "Right Stick Hold";
            if (immersive && any(key, "Delete", "Del"))
                return "Chest + Left Grip";
            if (first && same(key, "Z"))
                return vr_input_holster_combat_held() ? action_trigger
                                                      : "Hip Grip";

            if (style == VrControllerWmr)
            {
                if (any(key, "Space", "Spacebar")) return "Right Pad";
                if (same(key, "F"))                return "Left Pad";
                if (same(key, "Z"))                return "Left Menu";
                if (same(key, "K"))                return "Right Menu";
                if (any(key, "Delete", "Del"))   return "Right Menu Hold";
            }
            else if (style == VrControllerIndex)
            {
                if (any(key, "Space", "Spacebar")) return "Right A";
                if (same(key, "F"))                return "Right B";
                if (same(key, "Z"))                return "Left A";
                if (same(key, "K"))                return "Left B";
                if (any(key, "Delete", "Del"))   return "Left B Hold";
            }
            else
            {
                // HP Reverb G2 and Oculus Touch share these face-button
                // names, and this is also the safest fallback before SteamVR
                // has reported a controller type.
                if (any(key, "Space", "Spacebar")) return "Right A";
                if (same(key, "F"))                return "Right B";
                if (same(key, "Z"))                return "Left X";
                if (same(key, "K"))                return "Left Y";
                if (any(key, "Delete", "Del"))   return "Left Y Hold";
            }
            return nullptr;
        }

        GameUtf8String* __fastcall detour(void* self, void*, int button)
        {
            GameUtf8String* original = g_original(self, button);
            if (!config().controllers || !original || original->bytes == 0 ||
                original->bytes > 128 || !original->data)
                return original;

            const char* name = (const char*)original->data;
            const char* text = replacement(name, vr_input_controller_style());
            GameUtf8String* changed = text ? label(text) : nullptr;
            return changed ? changed : original;
        }

        // The pause menu's Widescreen toggle (useless in VR, the mod sizes
        // the view) becomes "VR Holster Setup" (user, 2026-10-04): its text
        // is swapped in the string table while the pause menu is up, and
        // RenderG2_SetWideScreen (0x0040CA30; the toggle at 0x00426990
        // calls it with !current) keeps the current value and asks for
        // gear placement instead.
        char g_holster_label[] = "VR Holster Setup";
        int g_wide_id = -2;              // -2 not searched, -1 none
        const char* g_wide_original = nullptr;

        const char** string_table(int* count)
        {
            __try
            {
                const char** table =
                    *reinterpret_cast<const char** const*>(0x01111EE4);
                const unsigned char* head =
                    *reinterpret_cast<const unsigned char* const*>(0x01111EE0);
                *count = table && head
                    ? *reinterpret_cast<const int*>(head + 4) : 0;
                return table;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                *count = 0;
                return nullptr;
            }
        }

        void holster_label(bool pause)
        {
            int count = 0;
            const char** table = string_table(&count);
            if (!table || count <= 0)
                return;
            __try
            {
                if (g_wide_id == -2)
                {
                    g_wide_id = -1;
                    for (int id = 0; id < count && id < 20000; ++id)
                    {
                        const char* s = table[id];
                        if (!s || IsBadStringPtrA(s, 32))
                            continue;
                        char lower[32]{};
                        const size_t n = strnlen(s, 31);
                        for (size_t i = 0; i < n; ++i)
                            lower[i] = (char)tolower((unsigned char)s[i]);
                        if (n <= 20 && strstr(lower, "wide") &&
                            strstr(lower, "screen"))
                        {
                            g_wide_id = id;
                            g_wide_original = s;
                            log("menu: Widescreen is string %d \"%s\"; "
                                "shown as \"%s\" in the pause menu", id, s,
                                g_holster_label);
                            break;
                        }
                    }
                }
                if (g_wide_id < 0 || g_wide_id >= count)
                    return;
                const char* want = pause ? g_holster_label : g_wide_original;
                if (want && table[g_wide_id] != want &&
                    (table[g_wide_id] == g_holster_label ||
                     table[g_wide_id] == g_wide_original))
                    table[g_wide_id] = want;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        typedef void (__cdecl* SetWideScreenFn)(bool);
        SetWideScreenFn g_set_wide = nullptr;
        bool g_wide_menu_setup = false;
        bool g_wide_forced_reported = false;

        // Widescreen is forced off in VR (user, 2026-10-04: narrow UI and
        // squashed intro movies). The flag only reshapes the 2D UI and
        // movies for a 16:9 monitor; each eye here is about square and the
        // 3D view is the mod's own, so "on" squeezes them. The registry
        // value is shared with the GOG copy and was found set to 1.
        void __cdecl detour_set_wide(bool on)
        {
            if (on && !g_wide_forced_reported)
            {
                g_wide_forced_reported = true;
                log("menu: widescreen request ignored -- kept off in VR "
                    "(each eye is about square)");
            }
            if (g_wide_menu_setup && ui_pause_menu_active() && on)
                tune_gear_tuning_request_from_menu();
            g_set_wide(false);
        }

        typedef int (__cdecl* MenuCommandFn)(void*, void*, int, const char*,
                                             int);
        MenuCommandFn g_push_screen = nullptr;

        int __cdecl detour_push_screen(void* menu, void* item, int screen,
                                       const char* text, int control)
        {
            holster_label(ui_root_screen_id() == 1);
            return g_push_screen(menu, item, screen, text, control);
        }
    }

    void input_labels_init()
    {
        // push esi / mov esi,[esp+8] -- two complete instructions.
        const unsigned char sig[] = { 0x56, 0x8B, 0x74, 0x24, 0x08 };
        if (hook_install((void*)kGetButtonName, (void*)&detour,
                         (void**)&g_original, sig, sizeof(sig),
                         "InputRemapDevice::GetButtonName"))
        {
            log("controls: tutorial key tokens use VR controller labels");
        }
        // cmp dword ptr [esp+14h], 5 -- one complete instruction.
        const unsigned char push_sig[] = { 0x83, 0x7C, 0x24, 0x14, 0x05 };
        hook_install((void*)0x004E4590, (void*)&detour_push_screen,
                     (void**)&g_push_screen, push_sig, sizeof(push_sig),
                     "menucommand_PushScreen (pause-menu holster label)");
        // mov ecx, [0x010FC914] -- one complete instruction.
        const unsigned char wide_sig[] =
            { 0x8B, 0x0D, 0x14, 0xC9, 0x0F, 0x01 };
        g_wide_menu_setup = config().immersive_controls;
        if (hook_install((void*)0x0040CA30, (void*)&detour_set_wide,
                         (void**)&g_set_wide, wide_sig, sizeof(wide_sig),
                         "RenderG2_SetWideScreen (kept off; pause-menu "
                         "holster setup)"))
        {
            // Already applied from the registry before the hook went in?
            // Clear the flag only; the projection it feeds is the mod's.
            bool* wide = reinterpret_cast<bool*>(0x00F48B0C);
            if (*wide)
            {
                *wide = false;
                log("menu: widescreen was on (registry) -- switched off "
                    "for VR");
            }
        }
    }
}
