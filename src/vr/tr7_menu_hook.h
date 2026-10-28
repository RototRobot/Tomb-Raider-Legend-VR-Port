#pragma once

// TR7-Menu-Hook (another mod, its menu on F8) folded into the settings
// overlay (1.1.0, user 2026-10-27: its options inside our panel, not its
// window). Its player / time / display items are the game's own functions
// and globals (the addresses TR7-Menu-Hook uses for this build, checked
// against our symbol remap); its saved options are its registry values,
// which it reads when the game starts.

namespace trlvr
{
    bool tr7_menu_hook_present();

    // Requested from the overlay, done by tr7_menu_hook_frame at the start
    // of the next Present -- not while the overlay's ImGui frame is being
    // built (user 2026-10-27: Next outfit crashed: the switch loads a model,
    // the game presents during the load, and that re-entered the overlay's
    // frame, so the widget after the button found no window).
    void tr7_refill_health();
    void tr7_next_outfit();
    void tr7_menu_hook_frame();
    // Null when the game's tracker is not there.
    float* tr7_time_multiplier();
    // The game's own switches TR7-Menu-Hook shows.
    enum Tr7Flag { Tr7Wireframe, Tr7HideUi, Tr7DebugKeypad, Tr7FlagCount };
    bool tr7_flag(Tr7Flag flag, bool* value);
    void tr7_set_flag(Tr7Flag flag, bool value);

    // Its saved options (HKCU\SOFTWARE\TRAE-Menu-Hook), with its defaults
    // when unset.
    int tr7_option_int(const char* name, int fallback);
    void tr7_set_option_int(const char* name, int value);
    bool tr7_option_bool(const char* name, bool fallback);
    void tr7_set_option_bool(const char* name, bool value);
    float tr7_option_float(const char* name, float fallback);
    void tr7_set_option_float(const char* name, float value);
}
