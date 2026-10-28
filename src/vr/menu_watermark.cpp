#include "menu_watermark.h"

#include "vr_backend.h"
#include "../common/log.h"
#include "../common/version.h"

#include <windows.h>

// "TRL VR 1.1.0 (OpenXR)" on the main menu (user 2026-10-27, after
// TR7-Menu-Hook's own watermark). The game's font queues text and draws it at
// its next Font::Flush, so printing here -- at present, after this frame's
// flush -- shows it from the next frame on, without hooking Font::Flush
// (TR7-Menu-Hook hooks that). Addresses are the retail build's (TR7-Menu-Hook
// uses the same; PrintFormatted / SetScale confirmed by our symbol remap,
// SetCursor by its code: it stores x, y at 0x0107F68C / 0x0107F690).

namespace trlvr
{
    namespace
    {
        const uintptr_t kMainState = 0x010E5868;        // GameTracker main state
        const int kMainStateMainMenu = 6;               // MS_DISPLAY_MAIN_MENU
        const uintptr_t kMainFont = 0x0107F680;         // Font* (main font)
        const uintptr_t kFontSetCursor = 0x00431670;    // static (float x, float y)
        const uintptr_t kFontPrintFormatted = 0x004323D0; // thiscall (text, backdrop)

        using PFN_SetCursor = void(__cdecl*)(float, float);
        using PFN_PrintFormatted = void(__thiscall*)(void*, const char*, int);

        bool g_failed = false;
    }

    void menu_watermark_frame()
    {
        if (g_failed)
            return;
        __try
        {
            if (*reinterpret_cast<const int*>(kMainState) != kMainStateMainMenu)
                return;
            void* font = *reinterpret_cast<void**>(kMainFont);
            if (!font)
                return;
            const VrBackend backend = vr_backend();
            const char* text = backend == VrBackendOpenXr
                ? "TRL VR " TRLVR_VERSION " (OpenXR)"
                : backend == VrBackendOpenVr
                    ? "TRL VR " TRLVR_VERSION " (OpenVR)"
                    : "TRL VR " TRLVR_VERSION " (no headset)";
            // TR7-Menu-Hook prints at (5, 430) in the 640x480 layout; this
            // goes on the line above it.
            reinterpret_cast<PFN_SetCursor>(kFontSetCursor)(5.0f, 414.0f);
            reinterpret_cast<PFN_PrintFormatted>(kFontPrintFormatted)(font, text, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_failed = true;
            log("menu: version watermark faulted; left off for this session");
        }
    }
}
