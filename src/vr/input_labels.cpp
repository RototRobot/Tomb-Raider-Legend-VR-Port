#include "input_labels.h"

#include "camera_head.h"
#include "hook.h"
#include "tune.h"
#include "ui_space.h"
#include "vr_input.h"
#include "vr_session.h"

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
            { "Right Stick Double", {}, false },
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
            if (same(key, "Tab"))
                return "Right Stick Double";
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

        // Pause-menu takeovers. Labels are swapped in the game's string table
        // (localstr_get 0x004E43C0 reads [0x01111EE4][id]) while a level is
        // running -- the only menu there is the pause menu -- and restored in
        // the front end, so the main menu keeps its own entries:
        //   Widescreen (useless in VR, the mod sizes the view) -> "VR Holster
        //     Setup" (user, 2026-10-04). RenderG2_SetWideScreen (0x0040CA30;
        //     the toggle at 0x00426990 calls it with !current) keeps the
        //     value and asks for gear placement instead.
        //   Display -> "VR Settings", and Next Generation Content (an
        //     unsupported renderer in VR) -> "Hand Calibration" (user,
        //     2026-10-06); menucommand_PcSwitchNextgenContent (0x004E5560)
        //     asks for hand calibration instead of switching.
        // Every matching string is swapped: the first build took only the
        // first "Next Generation Content" (string 59) and the pause menu kept
        // its retail text, so the menu draws another copy (user test
        // 2026-10-06).
        struct LabelSwap
        {
            int kind;
            int id;
            const char* original;
        };
        const char* kSwapNames[3] = {
            "Widescreen", "Display", "Next Generation Content" };
        char g_holster_label[] = "VR Holster Setup";
        char g_settings_label[] = "VR Settings";
        char g_calibration_label[] = "Hand Calibration";
        char* const kSwapLabels[3] = {
            g_holster_label, g_settings_label, g_calibration_label };
        LabelSwap g_swaps[16]{};
        int g_swap_count = 0;
        bool g_labels_searched = false;

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

        // IsFrontEnd (0x004E5980): the game state at 0x010E5868 is 6 in
        // the front end.
        bool in_level()
        {
            __try
            {
                return *reinterpret_cast<const volatile int*>(0x010E5868) != 6;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // Which takeover a lower-cased table string is: Widescreen, the
        // exact word Display, or a short Next Generation string (longer
        // ones are its help and restart texts).
        int label_kind(const char* lower, size_t n)
        {
            if (n <= 20 && strstr(lower, "wide") && strstr(lower, "screen"))
                return 0;
            if (strcmp(lower, "display") == 0)
                return 1;
            if (n <= 46 && strstr(lower, "next") && strstr(lower, "gen"))
                return 2;
            return -1;
        }

        void find_labels(const char** table, int count)
        {
            g_labels_searched = true;
            for (int id = 0; id < count && id < 20000; ++id)
            {
                const char* s = table[id];
                if (!s || IsBadStringPtrA(s, 48))
                    continue;
                char lower[48]{};
                const size_t n = strnlen(s, 47);
                if (n >= 47)
                    continue;
                for (size_t i = 0; i < n; ++i)
                    lower[i] = (char)tolower((unsigned char)s[i]);
                const int kind = label_kind(lower, n);
                if (kind < 0 || g_swap_count >= (int)_countof(g_swaps))
                    continue;
                g_swaps[g_swap_count++] = { kind, id, s };
                log("menu: %s is string %d \"%s\"; shown as \"%s\" in the "
                    "pause menu", kSwapNames[kind], id, s, kSwapLabels[kind]);
            }
            for (int kind = 0; kind < 3; ++kind)
            {
                bool found = false;
                for (int i = 0; i < g_swap_count; ++i)
                    found = found || g_swaps[i].kind == kind;
                if (!found)
                    log("menu: no \"%s\" string found; that pause-menu entry "
                        "keeps its retail text", kSwapNames[kind]);
            }
        }

        // VR labels in a level, retail labels in the front end.
        void update_labels()
        {
            if (!config().immersive_controls)
                return;
            int count = 0;
            const char** table = string_table(&count);
            if (!table || count <= 0)
                return;
            const bool vr = in_level();
            __try
            {
                if (!g_labels_searched)
                    find_labels(table, count);
                for (int i = 0; i < g_swap_count; ++i)
                {
                    const LabelSwap& swap = g_swaps[i];
                    const char* label = kSwapLabels[swap.kind];
                    if (swap.id < 0 || swap.id >= count)
                        continue;
                    const char* want = vr ? label : swap.original;
                    if (table[swap.id] != want &&
                        (table[swap.id] == label ||
                         table[swap.id] == swap.original))
                        table[swap.id] = want;
                }
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
            // The menu entry flips a game value (its On/Off) and calls this
            // through 0x004525D0 with the new value, so presses alternate
            // on/off. Reacting to "on" only made every other press do
            // nothing (tester report 2026-10-06, entry left showing "On").
            // A call while the pause menu's settings page (screen 2, where
            // the entry is) is on top is a press (300 ms debounce). Loading
            // a save from the pause menu re-applies options through here
            // too, which started holster setup (user 2026-10-06).
            static ULONGLONG last_press = 0;
            const ULONGLONG now = GetTickCount64();
            if (g_wide_menu_setup && ui_pause_menu_active() &&
                vr_input_top_screen_id() == 2 && now - last_press > 300)
            {
                last_press = now;
                log("menu: VR Holster Setup pressed (widescreen %s "
                    "requested)", on ? "on" : "off");
                tune_gear_tuning_request_from_menu();
            }
            g_set_wide(false);
        }

        typedef int (__cdecl* MenuCommandFn)(void*, void*, int, const char*,
                                             int);
        MenuCommandFn g_push_screen = nullptr;

        int __cdecl detour_push_screen(void* menu, void* item, int screen,
                                       const char* text, int control)
        {
            update_labels();
            return g_push_screen(menu, item, screen, text, control);
        }

        MenuCommandFn g_switch_nextgen = nullptr;

        // Next Generation Content in a level: hand calibration instead (on
        // select or left/right). The front end keeps retail.
        int __cdecl detour_switch_nextgen(void* menu, void* item, int screen,
                                          const char* text, int control)
        {
            if (!config().immersive_controls || !in_level())
                return g_switch_nextgen(menu, item, screen, text, control);
            static int reports = 0;
            if (reports < 4)
            {
                ++reports;
                char shown[64]{};
                if (text && !IsBadStringPtrA(text, 63))
                    strncpy_s(shown, text, _TRUNCATE);
                log("menu: Next Generation entry, control %d, item text "
                    "\"%s\"", control, shown);
            }
            // Select or left/right: on a toggle entry players press either
            // (retail reacts to 3, 4 and 5 alike).
            static ULONGLONG last_press = 0;
            const ULONGLONG now = GetTickCount64();
            if ((control == 5 || control == 3 || control == 4) &&
                now - last_press > 300)
            {
                last_press = now;
                vr_hand_calibration_request_from_menu();
            }
            return 0;
        }
    }

    void input_labels_update()
    {
        update_labels();
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
        // mov eax, [esp+14h] / cmp eax, 5 -- two complete instructions.
        const unsigned char nextgen_sig[] =
            { 0x8B, 0x44, 0x24, 0x14, 0x83, 0xF8, 0x05 };
        if (config().immersive_controls)
            hook_install((void*)0x004E5560, (void*)&detour_switch_nextgen,
                         (void**)&g_switch_nextgen, nextgen_sig,
                         sizeof(nextgen_sig),
                         "menucommand_PcSwitchNextgenContent (pause-menu "
                         "hand calibration)");
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
