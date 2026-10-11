#include "input_labels.h"
#include "lang.h"

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
            unsigned generation;
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
                // In the game's language (lang.cpp); rebuilt when it
                // changes (the old game string is left, a few bytes).
                if (!item.ready || item.generation != lang_generation())
                {
                    game_string_ctor(&item.value, tr(item.text));
                    item.ready = true;
                    item.generation = lang_generation();
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

        // The key an action is bound to, as the English name replacement()
        // knows, from its code -- the same in every language (2026-10-11:
        // localised key names such as "Espace" or "Suppr" matched nothing).
        // GetButtonName(action) reads the code at [this+0x94] + action*16;
        // SetDefaultMapping (0x004E2440) shows keyboard codes are 0x80 + the
        // DirectInput scan code (action 0 -> 0x92, E, as logged) and the
        // mouse buttons 0x60 / 0x61.
        const char* english_key_name(void* self, int button)
        {
            if (button < 0 || button > 255)
                return nullptr;
            unsigned code = 0;
            __try
            {
                const unsigned char* table =
                    *reinterpret_cast<unsigned char* const*>(
                        static_cast<unsigned char*>(self) + 0x94);
                if (!table)
                    return nullptr;
                code = *reinterpret_cast<const unsigned short*>(
                    table + button * 16);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return nullptr;
            }
            if (code == 0x60)
                return "Mouse 1";
            if (code < 0x80)
                return nullptr;
            switch (code - 0x80)
            {
            case 0x01: return "Escape";
            case 0x0F: return "Tab";
            case 0x10: return "Q";
            case 0x12: return "E";
            case 0x21: return "F";
            case 0x22: return "G";
            case 0x25: return "K";
            case 0x2C: return "Z";
            case 0x39: return "Space";
            case 0xC7: return "Home";
            case 0xCF: return "End";
            case 0xD1: return "Page Down";
            case 0xD3: return "Delete";
            default: return nullptr;
            }
        }

        GameUtf8String* __fastcall detour(void* self, void*, int button)
        {
            GameUtf8String* original = g_original(self, button);
            if (!config().controllers || !original || original->bytes == 0 ||
                original->bytes > 128 || !original->data)
                return original;

            const char* name = (const char*)original->data;
            // Which key number each (possibly localised) name belongs to,
            // once each: the table for matching keys by number in every
            // language (2026-10-10).
            {
                static int seen[64];
                static int seen_count = 0;
                bool known = false;
                for (int i = 0; i < seen_count; ++i)
                    known = known || seen[i] == button;
                if (!known && seen_count < 64)
                {
                    seen[seen_count++] = button;
                    const char* english = english_key_name(self, button);
                    log("controls: key token %d (0x%X) is \"%s\" (by its "
                        "code: %s)", button, button, name,
                        english ? english : "not a mapped key");
                }
            }
            // By name (English, unchanged), else by the bound key's code --
            // localised names ("Espace", "Suppr") are recognised that way.
            const char* text = replacement(name, vr_input_controller_style());
            if (!text)
            {
                const char* english = english_key_name(self, button);
                if (english)
                    text = replacement(english, vr_input_controller_style());
            }
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
        // The VR labels per game language (UTF-8, as the string table),
        // picked from what the game calls "Display" (string 653).
        struct LabelSet
        {
            const char* language;
            const char* display[3];     // lower-case words for "Display"
            const char* labels[3];
        };
        LabelSet kLabelSets[] = {
            { "English", { "display", nullptr, nullptr },
              { "VR Holster Setup", "VR Settings", "Hand Calibration" } },
            { "French", { "affichage", "\xC3\xA9" "cran", nullptr },
              { "Configuration de l'\xC3\xA9" "tui VR",
                "Param\xC3\xA8" "tres VR", "Calibrage des mains" } },
            { "German", { "anzeige", "bildschirm", "grafik" },
              { "VR-Holster einrichten", "VR-Einstellungen",
                "Handkalibrierung" } },
            { "Italian", { "visualizzazione", "schermo", "video" },
              { "Fondine VR", "Impostazioni VR", "Calibrazione mani" } },
            { "Spanish", { "pantalla", "visualizaci", "imagen" },
              { "Fundas VR", "Ajustes VR", "Calibrar manos" } },
        };
        LabelSet* g_label_set = &kLabelSets[0];
        const char* kSwapLabels[3] = {
            "VR Holster Setup", "VR Settings", "Hand Calibration" };
        // Their string ids in the English table (log 2026-10-10); a string
        // table keeps its ids in every language, only the text changes.
        struct KnownId { int kind; int id; };
        const KnownId kKnownIds[] = {
            { 0, 935 }, { 1, 653 }, { 2, 59 }, { 2, 755 } };
        LabelSwap g_swaps[16]{};
        int g_swap_count = 0;
        bool g_labels_searched = false;
        // Fullscreen Effects (string 25) -> "Hardcore Mode: Off / On" in a
        // level (user 2026-10-10). Pressing any Display-page entry runs
        // menucommand_PcTestDisplayMode, which rebuilds the device -- that
        // crashed the game in VR -- so this entry toggles hardcore first
        // person instead and never reaches it.
        // Found by its English text; -1 in a language not yet seen.
        int kHardcoreId = -1;
        const char* g_hardcore_original = nullptr;
        const char* g_hardcore_labels[2] = { "Hardcore Mode: Off",
                                             "Hardcore Mode: On" };

        // The other VR Settings entries (user 2026-10-10). Each takes over
        // one retail entry by its string id; Reflections' id is the one left
        // over once the page's items are read (calibrate_vr_menu). Labels
        // are "pause:" lang keys whose English is the text after the colon.
        int get_left_handed() { return config().left_handed ? 1 : 0; }
        void set_left_handed(int v) { config_set_left_handed(v != 0); }
        int get_theatre()
        {
            return config().gameplay_theatre ? 2
                : config().cutscene_theatre ? 1 : 0;
        }
        void set_theatre(int v) { config_set_theatre(v); }
        int get_third_person()
        {
            return config().view_cycle_all ? 3 : config().third_person_mode;
        }
        void set_third_person(int v)
        {
            config_set_third_person_mode(v);
            camera_set_third_person_mode(v);
        }
        int get_body() { return config().first_person_full_body ? 1 : 0; }
        void set_body(int v) { config_set_full_body(v != 0); }
        // Off, On (world), On + Body; Body Only is the old default and is
        // shown but not part of the cycle.
        int get_hand_collision()
        {
            const bool world = config().hand_collision;
            const bool body = config().hand_body_collision;
            return world ? (body ? 2 : 1) : (body ? 3 : 0);
        }
        void set_hand_collision(int v)
        {
            config_set_hand_collision(v == 1 || v == 2, v == 2 || v == 3);
        }

        struct VrToggle
        {
            const char* what;
            int retail_id;          // -1: the page's leftover (Reflections)
            int cycle;              // states a press steps through
            const char* keys[4];
            int (*get)();
            void (*set)(int);
            int id;                 // string id taken over, -1 not yet
            const char* original;
            const char* labels[4];
        };
        VrToggle g_toggles[] = {
            { "left-handed", -1, 2,
              { "pause:Left Handed: Off", "pause:Left Handed: On" },
              get_left_handed, set_left_handed },
            { "theatre", 25, 3,
              { "pause:Theatre: Off", "pause:Theatre: Cutscenes",
                "pause:Theatre: Always" },
              get_theatre, set_theatre },
            { "body", 26, 2,
              { "pause:Body: Hands", "pause:Body: Full" },
              get_body, set_body },
            { "third person", 637, 4,
              { "pause:Third Person: Classic", "pause:Third Person: Shoulder",
                "pause:Third Person: Board", "pause:Third Person: All" },
              get_third_person, set_third_person },
            { "hand collision", 675, 3,
              { "pause:Hand Collision: Off", "pause:Hand Collision: On",
                "pause:Hand Collision: On + Body",
                "pause:Hand Collision: Body Only" },
              get_hand_collision, set_hand_collision },
        };

        void resolve_toggle_labels()
        {
            for (VrToggle& t : g_toggles)
                for (int i = 0; i < 4; ++i)
                {
                    const char* key = t.keys[i];
                    if (!key)
                        continue;
                    const char* text = tr(key);
                    t.labels[i] = text != key ? text : key + 6;
                }
        }

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
            // Diagnostic (2026-10-08): the Display page's other entries, to
            // match against menucommand_PcCycleDisplayModeOption's items.
            for (int id = 0; id < count && id < 20000; ++id)
            {
                const char* s = table[id];
                if (!s || IsBadStringPtrA(s, 48))
                    continue;
                if (_stricmp(s, "Fullscreen Anti-Aliasing") == 0 ||
                    _stricmp(s, "Fullscreen Effects") == 0 ||
                    _stricmp(s, "Depth of field") == 0 ||
                    _stricmp(s, "Shadows") == 0)
                    log("menu: display entry \"%s\" is string %d", s, id);
            }
            // Not English: the entries by their known ids.
            for (int kind = 0; kind < 3; ++kind)
            {
                bool found = false;
                for (int i = 0; i < g_swap_count; ++i)
                    found = found || g_swaps[i].kind == kind;
                if (found)
                    continue;
                for (const KnownId& known : kKnownIds)
                {
                    if (known.kind != kind || known.id >= count ||
                        g_swap_count >= (int)_countof(g_swaps))
                        continue;
                    const char* s = table[known.id];
                    if (!s || IsBadStringPtrA(s, 64))
                        continue;
                    const size_t n = strnlen(s, 63);
                    if (n == 0 || n > 60)
                        continue;
                    g_swaps[g_swap_count++] = { kind, known.id, s };
                    found = true;
                    log("menu: %s taken by its string id %d (\"%s\" in this "
                        "language)", kSwapNames[kind], known.id, s);
                }
                if (!found)
                    log("menu: no \"%s\" string found; that pause-menu entry "
                        "keeps its retail text", kSwapNames[kind]);
            }
            g_hardcore_original = nullptr;
            kHardcoreId = -1;
            // The page's string ids hold in every language; only the retail
            // text is new (user test 2026-10-10: switching to French in a
            // level dropped the five toggles until the page was read again).
            for (VrToggle& t : g_toggles)
            {
                if (t.id < 0 || t.id >= count)
                    continue;
                const char* s = table[t.id];
                bool ours = false;
                for (const char* label : t.labels)
                    ours = ours || (label && s == label);
                if (!ours && s && !IsBadStringPtrA(s, 64))
                    t.original = s;
            }
            resolve_toggle_labels();
            for (int id = 0; id < count && id < 20000 && kHardcoreId < 0; ++id)
                if (table[id] && !IsBadStringPtrA(table[id], 32) &&
                    _stricmp(table[id], "Water Effects") == 0)
                    kHardcoreId = id;
            // Other languages: its id in the English table (log 2026-10-10;
            // a string table keeps its ids in every language).
            if (kHardcoreId < 0 && 816 < count)
                kHardcoreId = 816;
            if (kHardcoreId >= 0 && kHardcoreId < count && table[kHardcoreId] &&
                !IsBadStringPtrA(table[kHardcoreId], 64) &&
                strnlen(table[kHardcoreId], 63) > 0 &&
                strnlen(table[kHardcoreId], 63) <= 48)
            {
                g_hardcore_original = table[kHardcoreId];
                log("menu: Water Effects is string %d \"%s\"; shown as "
                    "the hardcore first-person toggle in the pause menu",
                    kHardcoreId, g_hardcore_original);
            }
            // The language, from the game's word for "Display".
            if (653 < count && table[653] && !IsBadStringPtrA(table[653], 64))
            {
                char lower[64]{};
                const size_t n = strnlen(table[653], 63);
                for (size_t i = 0; i < n; ++i)
                    lower[i] = (char)tolower((unsigned char)table[653][i]);
                for (LabelSet& set : kLabelSets)
                    for (const char* word : set.display)
                        if (word && strstr(lower, word))
                        {
                            g_label_set = &set;
                            goto picked;
                        }
            picked:
                // The user's translations (lang.cpp) where there are some.
                lang_set((int)(g_label_set - kLabelSets));
                static const char* const kPauseKeys[3] = {
                    "pause:VR Holster Setup", "pause:VR Settings",
                    "pause:Hand Calibration" };
                for (int kind = 0; kind < 3; ++kind)
                {
                    const char* text = tr(kPauseKeys[kind]);
                    if (text != kPauseKeys[kind])
                        g_label_set->labels[kind] = text;
                    kSwapLabels[kind] = g_label_set->labels[kind];
                }
                static const char* const kHardcoreKeys[2] = {
                    "pause:Hardcore Mode: Off", "pause:Hardcore Mode: On" };
                for (int on = 0; on < 2; ++on)
                {
                    const char* text = tr(kHardcoreKeys[on]);
                    g_hardcore_labels[on] = text != kHardcoreKeys[on] ? text
                        : (on ? "Hardcore Mode: On" : "Hardcore Mode: Off");
                }
                resolve_toggle_labels();
                log("menu: VR labels in %s (string 653 is \"%s\")",
                    g_label_set->language, table[653]);
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
                // The game's language changed in its options (user
                // 2026-10-10: switching to French kept the English labels):
                // the text table is reloaded, so look again.
                static const char** last_table = nullptr;
                static const char* last_display = nullptr;
                if (g_labels_searched && count > 653)
                {
                    const char* display = table[653];
                    bool ours = false;
                    for (const LabelSet& set : kLabelSets)
                        ours = ours || display == set.labels[1];
                    if (table != last_table ||
                        (!ours && display != last_display))
                    {
                        log("menu: the game's text changed (language?) -- "
                            "finding the entries again");
                        g_swap_count = 0;
                        g_labels_searched = false;
                    }
                }
                if (!g_labels_searched)
                {
                    find_labels(table, count);
                    last_table = table;
                    last_display = count > 653 ? table[653] : nullptr;
                }
                for (int i = 0; i < g_swap_count; ++i)
                {
                    const LabelSwap& swap = g_swaps[i];
                    const char* label = kSwapLabels[swap.kind];
                    // A label set picked after an earlier swap: put back
                    // whatever VR label is there first.
                    for (const LabelSet& set : kLabelSets)
                        if (table[swap.id] == set.labels[swap.kind] &&
                            set.labels[swap.kind] != label)
                            table[swap.id] = swap.original;
                    if (swap.id < 0 || swap.id >= count)
                        continue;
                    const char* want = vr ? label : swap.original;
                    if (table[swap.id] != want &&
                        (table[swap.id] == label ||
                         table[swap.id] == swap.original))
                        table[swap.id] = want;
                }
                if (g_hardcore_original && kHardcoreId >= 0 &&
                    kHardcoreId < count)
                {
                    const char* now = table[kHardcoreId];
                    const bool ours = now == g_hardcore_original ||
                        now == g_hardcore_labels[0] ||
                        now == g_hardcore_labels[1] ||
                        (now && (strcmp(now, "Hardcore Mode: Off") == 0 ||
                                 strcmp(now, "Hardcore Mode: On") == 0));
                    const char* want = vr
                        ? g_hardcore_labels[config().first_person_hardcore
                                            ? 1 : 0]
                        : g_hardcore_original;
                    if (ours && now != want)
                        table[kHardcoreId] = want;
                }
                for (VrToggle& t : g_toggles)
                {
                    if (t.id < 0 || t.id >= count || !t.original)
                        continue;
                    const char* now = table[t.id];
                    bool ours = now == t.original;
                    for (const char* label : t.labels)
                        ours = ours || (label && now == label);
                    const int state = t.get();
                    const char* label = state >= 0 && state < 4
                        ? t.labels[state] : nullptr;
                    const char* want = vr && label ? label : t.original;
                    if (ours && now != want)
                        table[t.id] = want;
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

        bool item_is_hardcore(void* item);
        void toggle_hardcore(const char* how);

        int __cdecl detour_push_screen(void* menu, void* item, int screen,
                                       const char* text, int control)
        {
            // Pressing a VR Settings (Display) entry pushes screen 4700, the
            // keep-these-settings check (user test 2026-10-10). Water
            // Effects in a level is the hardcore toggle: toggled here, the
            // screen is not pushed and the water setting never changes.
            if (screen == 4700 && in_level() && config().immersive_controls &&
                item_is_hardcore(item))
            {
                toggle_hardcore("pause menu, Water Effects entry");
                return 5;
            }
            update_labels();
            return g_push_screen(menu, item, screen, text, control);
        }

        MenuCommandFn g_switch_nextgen = nullptr;
        MenuCommandFn g_test_display = nullptr;

        // Whether a Display-page item is Fullscreen Effects: its words hold
        // the string id 25 or that text. The first presses of each item are
        // logged with its words, to check this.
        bool item_is_hardcore(void* item)
        {
            if (!item)
                return false;
            bool found = false;
            __try
            {
                const unsigned* w = static_cast<const unsigned*>(item);
                static void* logged[12] = {};
                bool known = false;
                int slot = -1;
                for (int i = 0; i < 12; ++i)
                {
                    known = known || logged[i] == item;
                    if (!logged[i] && slot < 0)
                        slot = i;
                }
                if (!known && slot >= 0)
                {
                    logged[slot] = item;
                    log("menu: display page item %p pressed: %08X %08X %08X "
                        "%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X "
                        "%08X %08X %08X", item, w[0], w[1], w[2], w[3], w[4],
                        w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12],
                        w[13], w[14], w[15]);
                }
                for (int i = 0; i < 16 && !found; ++i)
                {
                    const uintptr_t v = w[i];
                    found = (kHardcoreId >= 0 && v == (uintptr_t)kHardcoreId) ||
                        (g_hardcore_original &&
                         v == (uintptr_t)g_hardcore_original) ||
                        v == (uintptr_t)g_hardcore_labels[0] ||
                        v == (uintptr_t)g_hardcore_labels[1];
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            return found;
        }

        void toggle_hardcore(const char* how)
        {
            static ULONGLONG last_press = 0;
            const ULONGLONG now = GetTickCount64();
            if (now - last_press <= 300)
                return;
            last_press = now;
            const bool on = !config().first_person_hardcore;
            config_set_first_person_hardcore(on);
            log("menu: hardcore first person %s (%s)", on ? "ON" : "OFF", how);
            update_labels();
        }

        // The graphics settings block (see hook.cpp): FSAA +0x0A, depth of
        // field +0x0C, shadows +0x0E, reflection +0x2A, water +0x2B. Copied
        // every frame on the VR Settings page, so a press that changes it
        // can be undone before the device is rebuilt.
        unsigned char* const kGraphicsSettings =
            reinterpret_cast<unsigned char*>(0x00F12008);
        const int kGraphicsSettingsSize = 0x30;
        unsigned char g_settings_before[kGraphicsSettingsSize]{};
        bool g_settings_before_valid = false;

        void settings_snapshot()
        {
            const int index = *(volatile const int*)0x00F16E90;
            if (index < 0 || vr_input_top_screen_id() != 2)
            {
                g_settings_before_valid = false;
                return;
            }
            __try
            {
                memcpy(g_settings_before, kGraphicsSettings,
                       kGraphicsSettingsSize);
                g_settings_before_valid = true;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                g_settings_before_valid = false;
            }
        }

        // Menu::Select (0x004F55F0, thiscall, ret 8): the press on a menu.
        // The selected item is items[[menu +0x14] +4] (menu +0x24 count,
        // +0x28 items); its +0x14 word picks the item callback (14 is the
        // keep-settings check) and +0x18 is that callback's argument.
        typedef void* (__fastcall* MenuSelectFn)(void*, void*, int, int);
        MenuSelectFn g_menu_select = nullptr;
        struct MenuPress
        {
            const unsigned char* menu;
            const unsigned char* item;
            int index;
            int count;
            int callback;
            ULONGLONG tick;
        };
        MenuPress g_presses[4]{};
        int g_press_count = 0;

        void* __fastcall detour_menu_select(void* menu, void* edx, int a,
                                            int b)
        {
            const ULONGLONG now = GetTickCount64();
            if (g_press_count > 0 &&
                now - g_presses[g_press_count - 1].tick > 250)
                g_press_count = 0;
            __try
            {
                const unsigned char* m =
                    static_cast<const unsigned char*>(menu);
                const unsigned char* action =
                    *(const unsigned char* const*)(m + 0x14);
                if (action && g_press_count < 4)
                {
                    MenuPress& p = g_presses[g_press_count++];
                    p.menu = m;
                    p.index = *(const int*)(action + 4);
                    p.count = *(const int*)(m + 0x24);
                    p.item = (p.index >= 0 && p.index < p.count &&
                              p.count <= 64)
                        ? *(const unsigned char* const*)(m + 0x28 + p.index * 4)
                        : nullptr;
                    p.callback = p.item ? *(const short*)(p.item + 0x14) : -1;
                    p.tick = now;
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return g_menu_select(menu, edx, a, b);
        }

        // Whether an item's first 0x80 bytes hold the Water Effects string
        // id (as a 16- or 32-bit value) or its text pointer.
        bool item_mentions_water(const void* item)
        {
            if (!item)
                return false;
            __try
            {
                const unsigned char* p = static_cast<const unsigned char*>(item);
                for (int i = 0; i + 4 <= 0x80; i += 2)
                {
                    const unsigned short h = *(const unsigned short*)(p + i);
                    const uintptr_t w = *(const unsigned*)(p + i);
                    if ((kHardcoreId > 0 && h == (unsigned short)kHardcoreId) ||
                        (g_hardcore_original &&
                         w == (uintptr_t)g_hardcore_original) ||
                        w == (uintptr_t)g_hardcore_labels[0] ||
                        w == (uintptr_t)g_hardcore_labels[1])
                        return true;
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return false;
        }

        void log_item(const char* what, const void* item)
        {
            __try
            {
                const unsigned* w = static_cast<const unsigned*>(item);
                for (int row = 0; row < 2; ++row)
                {
                    const unsigned* r = w + row * 16;
                    log("menu:   %s %p +%02X: %08X %08X %08X %08X %08X %08X "
                        "%08X %08X %08X %08X %08X %08X %08X %08X %08X %08X",
                        what, item, row * 64, r[0], r[1], r[2], r[3], r[4],
                        r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12],
                        r[13], r[14], r[15]);
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        // Water Effects is the bottom entry of the VR Settings page that
        // opens the keep-settings check (callback 14). The menu has items
        // after it that do not (user test 2026-10-10: Water Effects is
        // entry 13 of 15). The menu's items are logged once, as the map for
        // the other VR toggles.
        int last_keep_entry(const unsigned char* menu, int count)
        {
            int last = -1;
            __try
            {
                static const unsigned char* logged = nullptr;
                const bool dump = menu != logged;
                logged = menu;
                for (int i = 0; i < count && i < 64; ++i)
                {
                    const unsigned char* it =
                        *(const unsigned char* const*)(menu + 0x28 + i * 4);
                    if (!it)
                        continue;
                    const short callback = *(const short*)(it + 0x14);
                    if (callback == 14)
                        last = i;
                    if (dump)
                        log("menu:   VR Settings item %2d: callback %d, +04 "
                            "%08X +0C %08X +18 %08X +20 %08X +44 %08X", i,
                            callback, *(const unsigned*)(it + 0x04),
                            *(const unsigned*)(it + 0x0C),
                            *(const unsigned*)(it + 0x18),
                            *(const unsigned*)(it + 0x20),
                            *(const unsigned*)(it + 0x44));
                }
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return last;
        }

        // Reading the VR Settings page. The page is a sub-menu (item +0x60)
        // of the screen's menu (screen +8); its keep-settings items
        // (callback 14) are the six display entries, Water Effects last.
        // Which text each shows is found from Water Effects: the place in
        // its item (or one pointer away) that holds its string id is read
        // from the others too, and accepted when the ids are distinct, real
        // strings, and include the known display entries.
        const unsigned char* g_vr_menu = nullptr;
        const unsigned char* g_vr_items[8]{};
        int g_vr_item_ids[8]{};
        int g_vr_item_count = 0;

        const unsigned char* find_vr_menu()
        {
            const int top = *(volatile const int*)0x00F16E90;
            if (top < 0 || top > 63)
                return nullptr;
            const unsigned char* screen =
                ((const unsigned char* const*)0x01116158)[top];
            const unsigned char* root = screen
                ? *(const unsigned char* const*)(screen + 8) : nullptr;
            if (!root)
                return nullptr;
            const int n = *(const int*)(root + 0x24);
            for (int i = 0; i < n && i < 64; ++i)
            {
                const unsigned char* it =
                    *(const unsigned char* const*)(root + 0x28 + i * 4);
                const unsigned char* sub = it
                    ? *(const unsigned char* const*)(it + 0x60) : nullptr;
                if (!sub)
                    continue;
                const int m = *(const int*)(sub + 0x24);
                int keep = 0;
                for (int k = 0; k < m && k < 64; ++k)
                {
                    const unsigned char* si =
                        *(const unsigned char* const*)(sub + 0x28 + k * 4);
                    keep += si && *(const short*)(si + 0x14) == 14;
                }
                if (keep >= 5)
                    return sub;
            }
            return nullptr;
        }

        bool readable(const void* p, size_t n)
        {
            return (uintptr_t)p >= 0x10000 && !IsBadReadPtr(p, n);
        }

        // outer: byte offset in the item; inner: -1 for the item itself,
        // else the offset behind the pointer at outer.
        int read_id(const unsigned char* item, int outer, int inner)
        {
            if (inner < 0)
                return *(const unsigned short*)(item + outer);
            const unsigned char* q =
                *(const unsigned char* const*)(item + outer);
            if (!readable(q, inner + 2))
                return -1;
            return *(const unsigned short*)(q + inner);
        }

        bool ids_plausible(const int* ids, int n, const char** table,
                           int count)
        {
            int known = 0;
            for (int i = 0; i < n; ++i)
            {
                if (ids[i] <= 0 || ids[i] >= count)
                    return false;
                for (int j = 0; j < i; ++j)
                    if (ids[j] == ids[i])
                        return false;
                const char* s = table[ids[i]];
                if (!s || IsBadStringPtrA(s, 64) || !*s)
                    return false;
                for (const VrToggle& t : g_toggles)
                    known += t.retail_id == ids[i];
            }
            return known >= 3;
        }

        // Once per page opening (the menu is rebuilt each time).
        void calibrate_vr_menu()
        {
            const unsigned char* menu = find_vr_menu();
            if (!menu || menu == g_vr_menu)
                return;
            g_vr_menu = menu;
            g_vr_item_count = 0;
            int count = 0;
            const char** table = string_table(&count);
            if (!table || count <= 0)
                return;
            const unsigned char* items[8]{};
            int n = 0;
            const int m = *(const int*)(menu + 0x24);
            for (int k = 0; k < m && k < 64 && n < 8; ++k)
            {
                const unsigned char* it =
                    *(const unsigned char* const*)(menu + 0x28 + k * 4);
                if (it && *(const short*)(it + 0x14) == 14)
                    items[n++] = it;
            }
            if (n < 2)
                return;
            const unsigned char* water = items[n - 1];
            const int water_id = kHardcoreId >= 0 ? kHardcoreId : 816;
            int ids[8]{};
            for (int outer = 0; outer < 0x80; outer += 2)
            {
                for (int inner = -1; inner < 0x40; inner += inner < 0 ? 1 : 2)
                {
                    if (inner >= 0 && (outer & 3))
                        break;
                    if (read_id(water, outer, inner) != water_id)
                        continue;
                    for (int i = 0; i < n; ++i)
                        ids[i] = read_id(items[i], outer, inner);
                    if (!ids_plausible(ids, n, table, count))
                        continue;
                    for (int i = 0; i < n; ++i)
                    {
                        g_vr_items[i] = items[i];
                        g_vr_item_ids[i] = ids[i];
                    }
                    g_vr_item_count = n;
                    static bool reported = false;
                    if (!reported)
                        log("menu: VR Settings page read -- string ids at "
                            "item +%02X%s (inner +%02X); Water Effects is %d",
                            outer, inner >= 0 ? ", one pointer away" : "",
                            inner >= 0 ? inner : 0, water_id);
                    // Each entry's toggle; Reflections is the id no toggle
                    // names.
                    for (int i = 0; i < n; ++i)
                    {
                        const int id = ids[i];
                        if (id == water_id)
                            continue;
                        VrToggle* owner = nullptr;
                        for (VrToggle& t : g_toggles)
                            if (t.retail_id == id)
                                owner = &t;
                        if (!owner)
                            for (VrToggle& t : g_toggles)
                                if (t.retail_id < 0)
                                    owner = &t;
                        if (owner && owner->id != id)
                        {
                            owner->id = id;
                            owner->original = table[id];
                        }
                        if (!reported)
                            log("menu:   entry %d: string %d \"%s\" -> %s",
                                i + 1, id,
                                owner && owner->original ? owner->original
                                                         : "?",
                                owner ? owner->what : "(none)");
                    }
                    reported = true;
                    return;
                }
            }
            static bool failed_reported = false;
            if (failed_reported)
                return;
            failed_reported = true;
            log("menu: VR Settings page -- could not find the entries' "
                "string ids; only Water Effects (hardcore) is taken over. "
                "Water Effects item and what its pointers lead to:");
            log_item("water", water);
            for (int outer = 0; outer < 0x80; outer += 4)
            {
                const unsigned char* q =
                    *(const unsigned char* const*)(water + outer);
                if (readable(q, 0x80))
                    log_item("->", q);
            }
        }

        // The toggle a pressed VR Settings item stands for, if any.
        VrToggle* toggle_for_item(const unsigned char* item)
        {
            for (int i = 0; item && i < g_vr_item_count; ++i)
            {
                if (g_vr_items[i] != item)
                    continue;
                for (VrToggle& t : g_toggles)
                    if (t.id >= 0 && t.id == g_vr_item_ids[i])
                        return &t;
            }
            return nullptr;
        }

        void press_toggle(VrToggle& t)
        {
            static ULONGLONG last_press = 0;
            const ULONGLONG now = GetTickCount64();
            if (now - last_press <= 300)
                return;
            last_press = now;
            const int state = t.get();
            const int next =
                state >= 0 && state + 1 < t.cycle ? state + 1 : 0;
            t.set(next);
            log("menu: %s -> \"%s\" (pause menu)", t.what,
                t.labels[next] ? t.labels[next] : "?");
            update_labels();
        }

        typedef int (__cdecl* KeepCheckFn)(int);
        KeepCheckFn g_keep_check = nullptr;

        // The VR Settings (Display) page's select callback (0x004E5780,
        // run later by 0x004F7FD0 as callback(item +0x18)) pushes screen
        // 4700, the keep-these-settings check, which rebuilds the device --
        // in VR that crashed the game (user 2026-10-10). In a level: Water
        // Effects, the bottom entry, toggles hardcore first person; any
        // other entry is undone and blocked for now.
        int __cdecl detour_keep_check(int arg)
        {
            if (!in_level() || !config().immersive_controls ||
                vr_input_top_screen_id() != 2)
                return g_keep_check(arg);

            int changed[8]{};
            int changes = 0;
            bool water = false;
            if (g_settings_before_valid)
            {
                __try
                {
                    for (int i = 0; i < kGraphicsSettingsSize; ++i)
                    {
                        if (kGraphicsSettings[i] == g_settings_before[i])
                            continue;
                        if (changes < 8)
                            changed[changes] = i;
                        ++changes;
                        water = water || i == 0x2B;
                        kGraphicsSettings[i] = g_settings_before[i];
                    }
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
            log("menu: VR Settings press, callback argument %d; %d setting "
                "byte(s) changed and undone: +%02X +%02X +%02X +%02X", arg,
                changes, changed[0], changed[1], changed[2], changed[3]);

            // The pressed entry: the menu select that queued this callback.
            bool bottom = false;
            bool named = false;
            const unsigned char* pressed = nullptr;
            const bool recent = g_press_count > 0 &&
                GetTickCount64() - g_presses[g_press_count - 1].tick < 1000;
            for (int i = 0; recent && i < g_press_count; ++i)
            {
                const MenuPress& p = g_presses[i];
                const bool mentions = item_mentions_water(p.item);
                log("menu:   select on menu %p: entry %d of %d, item %p, "
                    "callback %d%s", p.menu, p.index, p.count, p.item,
                    p.callback,
                    mentions ? " (holds the Water Effects string)" : "");
                if (p.item)
                    log_item("item", p.item);
                if (p.callback == 14)
                {
                    pressed = p.item;
                    named = named || mentions;
                    bottom = bottom || p.index == last_keep_entry(p.menu,
                                                                  p.count);
                }
            }
            g_press_count = 0;
            if (VrToggle* toggle = toggle_for_item(pressed))
            {
                press_toggle(*toggle);
                return 1;
            }
            if (water || named || bottom)
                toggle_hardcore("pause menu, Water Effects entry");
            else
                log("menu: display change blocked in a level -- the keep-"
                    "settings check rebuilds the device, which VR cannot "
                    "survive");
            return 1;
        }

        // menucommand_PcTestDisplayMode (0x004E5030): select on a
        // Display-page entry applies the display mode (a device rebuild)
        // and opens the "keep these settings" screen. In a level the
        // Fullscreen Effects entry toggles hardcore first person instead.
        int __cdecl detour_test_display(void* menu, void* item, int screen,
                                        const char* text, int control)
        {
            if (control == 5 && in_level() && config().immersive_controls &&
                item_is_hardcore(item))
            {
                toggle_hardcore("pause menu, display test");
                return 5;
            }
            // Any other Display-page entry in a level: the device rebuild
            // crashed the game in VR (user 2026-10-10), so it is not run.
            if (control == 5 && in_level() && config().immersive_controls)
            {
                static int reports = 0;
                if (reports++ < 8)
                    log("menu: display mode change blocked in a level -- it "
                        "rebuilds the device, which VR cannot survive");
                return 5;
            }
            return g_test_display(menu, item, screen, text, control);
        }
        MenuCommandFn g_cycle_display = nullptr;

        // Diagnostic: menucommand_PcCycleDisplayModeOption (0x004E5090) is
        // shared by the Display page's toggles (anti-aliasing, shadows,
        // effects); its text argument is empty, so log each item's first
        // words once, to find the one to take over.
        int __cdecl detour_cycle_display(void* menu, void* item, int screen,
                                         const char* text, int control)
        {
            static void* seen[16] = {};
            if (control != 0 && item)
            {
                bool known = false;
                int slot = -1;
                for (int i = 0; i < 16; ++i)
                {
                    known = known || seen[i] == item;
                    if (!seen[i] && slot < 0)
                        slot = i;
                }
                if (!known && slot >= 0)
                {
                    seen[slot] = item;
                    __try
                    {
                        const unsigned* w =
                            static_cast<const unsigned*>(item);
                        log("menu: display option item %p control %d "
                            "screen %d: %08X %08X %08X %08X %08X %08X %08X "
                            "%08X %08X %08X %08X %08X %08X %08X %08X %08X",
                            item, control, screen, w[0], w[1], w[2], w[3],
                            w[4], w[5], w[6], w[7], w[8], w[9], w[10],
                            w[11], w[12], w[13], w[14], w[15]);
                    }
                    __except(EXCEPTION_EXECUTE_HANDLER)
                    {
                    }
                }
            }
            return g_cycle_display(menu, item, screen, text, control);
        }

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
        settings_snapshot();
        if (in_level() && config().immersive_controls &&
            vr_input_top_screen_id() == 2)
        {
            __try
            {
                calibrate_vr_menu();
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
            }
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
        // mov eax, [esp+14h] / cmp eax, 5 -- two complete instructions.
        const unsigned char nextgen_sig[] =
            { 0x8B, 0x44, 0x24, 0x14, 0x83, 0xF8, 0x05 };
        hook_install((void*)0x004E5090, (void*)&detour_cycle_display,
                     (void**)&g_cycle_display, nextgen_sig,
                     sizeof(nextgen_sig),
                     "menucommand_PcCycleDisplayModeOption (diagnostic)");
        // cmp dword ptr [esp+14h], 5 -- one complete instruction.
        if (config().immersive_controls)
            hook_install((void*)0x004E5030, (void*)&detour_test_display,
                         (void**)&g_test_display, push_sig, sizeof(push_sig),
                         "menucommand_PcTestDisplayMode (pause-menu hardcore "
                         "toggle)");
        // push ebx / push ebp / push esi / mov esi, ecx -- four complete
        // instructions.
        const unsigned char select_sig[] = { 0x53, 0x55, 0x56, 0x8B, 0xF1 };
        if (config().immersive_controls)
            hook_install((void*)0x004F55F0, (void*)&detour_menu_select,
                         (void**)&g_menu_select, select_sig,
                         sizeof(select_sig),
                         "Menu::Select (pause-menu hardcore toggle)");
        // push 0 / push 4700 -- two complete instructions.
        const unsigned char keep_sig[] =
            { 0x6A, 0x00, 0x68, 0x5C, 0x12, 0x00, 0x00 };
        if (config().immersive_controls)
            hook_install((void*)0x004E5780, (void*)&detour_keep_check,
                         (void**)&g_keep_check, keep_sig, sizeof(keep_sig),
                         "VR Settings keep-settings check (pause-menu "
                         "hardcore toggle)");
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
