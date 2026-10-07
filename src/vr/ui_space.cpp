// Classify the game's HUD batches at their owning BasicDrawable boundary.

#include "ui_space.h"
#include "perf_cpu.h"
#include "camera_head.h"
#include "hook.h"
#include "hud_capture.h"

#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>
#include <cstdint>

namespace trlvr
{
    namespace
    {
        constexpr uintptr_t kBasicDrawableDraw = 0x00415AB0;
        constexpr uintptr_t kHandleScreenWipes = 0x00450430;
        constexpr uintptr_t kSimpleDrawableVtable = 0x00EFE3E0;
        constexpr uintptr_t kHudDrawableVtable = 0x00EFE3F4;
        constexpr uintptr_t kScreenStackIndex = 0x00F16E90;
        constexpr uintptr_t kScreenStack = 0x01116158;
        constexpr short kPauseRootScreen = 1;
        constexpr uintptr_t kLooseLedgePromptTexturePage = 0x00002096;
        constexpr uintptr_t kCinematicBarsTexturePage = 0x00002000;
        constexpr unsigned char kDrawSignature[5] =
            { 0x53, 0x8B, 0x5C, 0x24, 0x08 };
        constexpr unsigned char kScreenWipeSignature[5] =
            { 0xA0, 0x53, 0x54, 0x0E, 0x01 };

        using PFN_Draw = void(__thiscall*)(void*, unsigned, void*);
        PFN_Draw g_original = nullptr;
        using PFN_ScreenWipes = void(__cdecl*)();
        PFN_ScreenWipes g_original_screen_wipes = nullptr;
        bool g_drawing_hud = false;
        bool g_drawing_world_marker = false;
        // 0x202F caution triangle, 0x204E/0x205A lock-on reticle ring and
        // arrows, 0x2096 grab-prompt hand.
        bool world_marker_page(uintptr_t page)
        {
            return page == 0x202F || page == 0x204E || page == 0x205A ||
                   page == 0x2096;
        }

        // Every world sprite the player code draws at a point in the world
        // (user, 2026-10-05: the grapple swing-target ring floated like the
        // caution icon once did). playerDrawSpriteScreen (0x005A5060) takes
        // its texture from [0x011186AC + type * 4]; types 0..0x1F are the
        // sense-target, caution and lock-on sprites, 0x20..0x23 the hit
        // flash wedges (screen-edge, not world). A page found in the first
        // range is a world marker. The plain white and font pages are never
        // taken from it, so solid HUD bars and text stay where they are.
        constexpr uintptr_t kWorldSpriteTextures = 0x011186AC;
        bool g_sprite_table_reported = false;
        uintptr_t g_sprite_pages_reported[16]{};
        unsigned g_sprite_pages_reported_count = 0;

        int world_sprite_type(uintptr_t page)
        {
            if (!page || page == 0x2000 || page == 0x2001 || page == 0x2002)
                return -1;
            __try
            {
                const uintptr_t* table =
                    reinterpret_cast<const uintptr_t*>(kWorldSpriteTextures);
                if (!g_sprite_table_reported && table[5])
                {
                    g_sprite_table_reported = true;
                    log("ui: world sprite textures 0-7: %X %X %X %X %X %X %X "
                        "%X; 8-15: %X %X %X %X %X %X %X %X", table[0],
                        table[1], table[2], table[3], table[4], table[5],
                        table[6], table[7], table[8], table[9], table[10],
                        table[11], table[12], table[13], table[14],
                        table[15]);
                    log("ui: world sprite textures 16-23: %X %X %X %X %X %X "
                        "%X %X; 24-31: %X %X %X %X %X %X %X %X", table[16],
                        table[17], table[18], table[19], table[20],
                        table[21], table[22], table[23], table[24],
                        table[25], table[26], table[27], table[28],
                        table[29], table[30], table[31]);
                }
                // The table holds the interface texture's index; the
                // drawable's page is 0x2000 + index (log 2026-10-05: 4E,
                // 5A, 2F for the known 0x204E, 0x205A, 0x202F).
                for (int type = 0; type < 0x20; ++type)
                    if (table[type] && (table[type] == page ||
                                        0x2000 + table[type] == page))
                        return type;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return -1;
        }
        bool g_world_locked_hud = false;
        bool g_drawing_screen_wipe = false;
        bool g_classification_ready = false;

        uintptr_t object_vtable(void* object)
        {
            if (!object)
                return 0;
            __try
            {
                return *reinterpret_cast<const uintptr_t*>(object);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        bool basic_fields(void* object, uintptr_t* texture_page,
                          unsigned* indices)
        {
            __try
            {
                const unsigned char* p = static_cast<const unsigned char*>(object);
                *texture_page = *reinterpret_cast<const uintptr_t*>(p + 0x0C);
                *indices = *reinterpret_cast<const unsigned*>(p + 0x1C);
                return true;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool validate_vtables()
        {
            __try
            {
                return *reinterpret_cast<const uintptr_t*>(kSimpleDrawableVtable + 4)
                           == kBasicDrawableDraw &&
                       *reinterpret_cast<const uintptr_t*>(kHudDrawableVtable + 4)
                           == kBasicDrawableDraw;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        int menu_screen_index()
        {
            __try
            {
                return *reinterpret_cast<volatile const int*>(kScreenStackIndex);
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return -1;
            }
        }

        bool menu_screen_active()
        {
            return menu_screen_index() >= 0;
        }

        short screen_id_at(int slot)
        {
            if (slot < 0 || slot > 63)
                return -1;
            __try
            {
                const short* const* stack =
                    reinterpret_cast<const short* const*>(kScreenStack);
                const short* entry = stack[slot];
                return entry ? *entry : (short)-1;
            }
            __except(EXCEPTION_EXECUTE_HANDLER)
            {
                return -1;
            }
        }

        bool pause_screen_active()
        {
            return menu_screen_active() && screen_id_at(0) == kPauseRootScreen;
        }

        void __cdecl screen_wipes_detour()
        {
            // Retail draws these 2D quads directly, outside HUDDrawable.
            // Keep the tag for their synchronous DrawQuads2D calls only.
            if (camera_first_person_active())
            {
                static DWORD last_report = 0;
                const DWORD now = GetTickCount();
                if (!last_report || now - last_report >= 5000)
                {
                    last_report = now;
                    const unsigned state =
                        *reinterpret_cast<volatile const unsigned char*>(
                            0x010E5453);
                    log("ui: first-person screen-wipe pass, retail state %u",
                        state);
                }
            }
            const bool outer = g_drawing_screen_wipe;
            g_drawing_screen_wipe = true;
            g_original_screen_wipes();
            g_drawing_screen_wipe = outer;
        }

        void __fastcall draw_detour(void* self, void*, unsigned pass, void* previous)
        {
            PerfCpuScope perf_scope(PerfUiDraw);
            const uintptr_t vtable = object_vtable(self);
            const bool hud = g_classification_ready &&
                             vtable == kHudDrawableVtable;

            uintptr_t texture_page = 0;
            unsigned indices = 0;
            const bool have_fields = basic_fields(self, &texture_page, &indices);
            const bool prompt = g_classification_ready && have_fields &&
                                vtable == kSimpleDrawableVtable &&
                                texture_page == kLooseLedgePromptTexturePage &&
                                indices == 6;
            const bool bars = g_classification_ready && have_fields && hud &&
                              texture_page == kCinematicBarsTexturePage &&
                              indices == 12;

            if (bars)
                return;

            const bool outer_hud = g_drawing_hud;
            const bool outer_world_locked = g_world_locked_hud;
            g_drawing_hud = hud || prompt;
            g_world_locked_hud = hud && config().menu_world_locked &&
                                 menu_screen_active() &&
                                 !pause_screen_active();
            const int previous_hand = camera_first_person_gpu_hand_draw(
                camera_first_person_drawable_hand(self));
            if (have_fields)
                hud_capture_set_drawable(texture_page, indices, hud ? 1 : 0);
            const bool outer_marker = g_drawing_world_marker;
            const int sprite_type = g_classification_ready && have_fields &&
                (hud || vtable == kSimpleDrawableVtable)
                ? world_sprite_type(texture_page) : -1;
            if (sprite_type >= 0 && !world_marker_page(texture_page))
            {
                bool seen = false;
                for (unsigned i = 0; i < g_sprite_pages_reported_count; ++i)
                    seen = seen || g_sprite_pages_reported[i] == texture_page;
                if (!seen && g_sprite_pages_reported_count < 16)
                {
                    g_sprite_pages_reported[
                        g_sprite_pages_reported_count++] = texture_page;
                    log("ui: world sprite type %d (page 0x%X, %u indices, "
                        "%s) drawn as a world marker", sprite_type,
                        (unsigned)texture_page, indices,
                        hud ? "HUDDrawable" : "SimpleDrawable");
                }
            }
            if (sprite_type >= 0)
                g_drawing_hud = true;
            g_drawing_world_marker = (hud || prompt || sprite_type >= 0) &&
                                     have_fields &&
                                     (world_marker_page(texture_page) ||
                                      sprite_type >= 0);
            g_original(self, pass, previous);
            g_drawing_world_marker = outer_marker;
            hud_capture_set_drawable(0, 0, -1);
            camera_first_person_gpu_hand_draw_end(previous_hand);
            g_drawing_hud = outer_hud;
            g_world_locked_hud = outer_world_locked;
        }
    }

    bool ui_drawing_world_marker()
    {
        return g_drawing_world_marker;
    }

    bool ui_drawing_interface()
    {
        return g_drawing_hud || g_drawing_screen_wipe;
    }

    bool ui_interface_world_locked()
    {
        return g_world_locked_hud && !g_drawing_screen_wipe;
    }

    bool ui_drawing_screen_wipe()
    {
        return g_drawing_screen_wipe;
    }

    bool ui_menu_active()
    {
        return menu_screen_active();
    }

    short ui_root_screen_id()
    {
        return menu_screen_active() ? screen_id_at(0) : (short)-1;
    }

    bool ui_loading_screen_active()
    {
        const short root = ui_root_screen_id();
        return root >= 2000 && root < 3000;
    }

    bool ui_mission_prep_active()
    {
        // Root 3001: Mission Prep (outfit / difficulty before a level), a
        // close menu shot of Lara in the manor library behind the menu.
        const short root = ui_root_screen_id();
        return root >= 3000 && root < 4000;
    }

    bool ui_pda_active()
    {
        // Root 6 from the 2026-10-06 log (Objectives 12, Data 10, Gear 9
        // open on top of it).
        return ui_root_screen_id() == 6;
    }

    bool ui_pause_menu_active()
    {
        return pause_screen_active();
    }

    void ui_space_init()
    {
        if (!validate_vtables())
        {
            log("ui: HUD classification disabled -- executable signature mismatch");
            return;
        }

        void* trampoline = nullptr;
        if (hook_install((void*)kBasicDrawableDraw, (void*)&draw_detour,
                         &trampoline, kDrawSignature, sizeof(kDrawSignature),
                         "BasicDrawable::Draw (HUD classifier)"))
        {
            g_original = reinterpret_cast<PFN_Draw>(trampoline);
            g_classification_ready = true;
            log("ui: production HUD/menu/prompt classifier enabled");
        }
        trampoline = nullptr;
        if (hook_install((void*)kHandleScreenWipes,
                         (void*)&screen_wipes_detour, &trampoline,
                         kScreenWipeSignature, sizeof(kScreenWipeSignature),
                         "GAMELOOP_HandleScreenWipes (VR overlay)"))
        {
            g_original_screen_wipes =
                reinterpret_cast<PFN_ScreenWipes>(trampoline);
            log("ui: retail screen wipes tagged as fullscreen VR overlays");
        }
    }
}
