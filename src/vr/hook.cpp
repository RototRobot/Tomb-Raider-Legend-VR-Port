#include "hook.h"
#include "../common/config.h"

#include "../common/log.h"

#include <cstdio>
#include <cstring>

namespace trlvr
{
    namespace
    {
        // Trampolines have to be executable, and must not move, so they get
        // their own page rather than living on the heap.
        unsigned char* alloc_exec(size_t bytes)
        {
            return (unsigned char*)VirtualAlloc(nullptr, bytes,
                                                MEM_COMMIT | MEM_RESERVE,
                                                PAGE_EXECUTE_READWRITE);
        }

        void write_jmp(unsigned char* at, const void* to)
        {
            at[0] = 0xE9;                                    // jmp rel32
            const intptr_t rel = (intptr_t)to - (intptr_t)at - 5;
            memcpy(at + 1, &rel, 4);
        }
    }

    bool hook_install(void* target, void* detour, void** trampoline,
                      const unsigned char* expect, size_t len,
                      const char* name)
    {
        if (!target || !detour || !trampoline || !expect || len < 5 || len > 32)
            return false;

        unsigned char* t = (unsigned char*)target;

        // Refuse rather than corrupt. A mismatch means the address is not the
        // function we think it is, and writing anyway would break the game
        // somewhere far away from the cause.
        if (memcmp(t, expect, len) != 0)
        {
            log("hook: %s NOT installed -- the bytes at 0x%p are not what was expected",
                name, target);
            log("      expected:");
            char line[128]{};
            int n = 0;
            for (size_t i = 0; i < len; ++i)
                n += sprintf_s(line + n, sizeof(line) - n, "%02X ", expect[i]);
            log("        %s", line);
            n = 0;
            line[0] = '\0';
            for (size_t i = 0; i < len; ++i)
                n += sprintf_s(line + n, sizeof(line) - n, "%02X ", t[i]);
            log("      found:");
            log("        %s", line);
            return false;
        }

        // A trampoline is a verbatim copy of the original bytes, so anything
        // position-dependent in them is wrong the moment it moves. call rel32,
        // jmp rel32 and jmp rel8 all encode a displacement from their own
        // address; copied elsewhere they point into whatever happens to be
        // there.
        //
        // This cost a crash to learn -- UIScreenManager::Process begins with
        // two relative calls, and the trampoline jumped into the heap
        // (EIP 0x1d584b00). Refusing is the right answer rather than
        // relocating: a hook that cannot be installed safely is a hook that
        // should be moved to a different function, and the log says which.
        //
        // Deliberately crude -- these bytes are only scanned, not decoded, so
        // an immediate or a ModRM byte that happens to be 0xE8 refuses a hook
        // that would have been fine. That is the safe direction to be wrong
        // in, and the fix is to choose a different prologue length.
        for (size_t i = 0; i < len; ++i)
        {
            if (t[i] == 0xE8 || t[i] == 0xE9 || t[i] == 0xEB)
            {
                log("hook: %s NOT installed -- byte %u of the prologue is 0x%02X, "
                    "a relative branch. Copying it into a trampoline would send "
                    "it somewhere else entirely.", name, (unsigned)i, t[i]);
                return false;
            }
        }

        unsigned char* tramp = alloc_exec(len + 5);
        if (!tramp)
        {
            log("hook: %s -- could not allocate a trampoline", name);
            return false;
        }

        memcpy(tramp, t, len);                 // the original instructions
        write_jmp(tramp + len, t + len);       // then back to what follows

        DWORD old = 0;
        if (!VirtualProtect(t, len, PAGE_EXECUTE_READWRITE, &old))
        {
            log("hook: %s -- could not unprotect 0x%p", name, target);
            VirtualFree(tramp, 0, MEM_RELEASE);
            return false;
        }

        write_jmp(t, detour);
        for (size_t i = 5; i < len; ++i)
            t[i] = 0x90;                       // pad the remainder with nops

        VirtualProtect(t, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), t, len);

        *trampoline = tramp;
        log("hook: %s installed at 0x%p", name, target);
        return true;
    }
    // [vr] depth_of_field (2026-10-05). The game loads its graphics
    // settings from the registry with 0x00EC3730 (key path, settings) into
    // the block at 0x00F12008, before any device exists; EnableDepthOfField
    // is the byte at +0x0C (read at 0x00EC3925, saved from [esi+0x0C]).
    // EnableFullscreenEffects is the byte at +0x0B (the save order of the
    // names at 0x00F08E50.. : VSync +0x09, FullscreenEffects +0x0B).
    // The loader runs before the game loads this DLL (2026-10-11: no log of
    // the hook in any session, and the registry's depth of field was in
    // effect -- the distance looked blurred), so the block is overwritten
    // directly when the DLL loads, and the hook stays for any later load.
    // The game saves the block back on exit, so the launcher then shows the
    // INI's choice.
    namespace
    {
        typedef bool (__cdecl* PFN_LoadGraphicsSettings)(const char*, void*);
        PFN_LoadGraphicsSettings g_load_graphics_settings = nullptr;

        void override_settings(void* settings, const char* when)
        {
            struct Flag { int value; unsigned offset; const char* name;
                          const char* key; };
            const Flag flags[3] = {
                { config().depth_of_field, 0x0C, "depth of field",
                  "depth_of_field" },
                { config().fullscreen_effects, 0x0B, "fullscreen effects",
                  "fullscreen_effects" },
                { config().fsaa, 0x0A, "anti-aliasing (FSAA)", "fsaa" } };
            for (const Flag& f : flags)
            {
                if (f.value < 0 || !settings)
                    continue;
                __try
                {
                    unsigned char* flag =
                        static_cast<unsigned char*>(settings) + f.offset;
                    const unsigned char was = *flag;
                    *flag = f.value ? 1 : 0;
                    log("settings: %s %s by [vr] %s (game setting was %s; "
                        "%s)", f.name, f.value ? "ON" : "OFF", f.key,
                        was ? "on" : "off", when);
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
        }

        bool __cdecl detour_load_graphics_settings(const char* key,
                                                   void* settings)
        {
            const bool loaded = g_load_graphics_settings(key, settings);
            override_settings(settings, "on load");
            return loaded;
        }
    }

    void hook_apply_settings_overrides()
    {
        if (config().depth_of_field < 0 && config().fullscreen_effects < 0 &&
            config().fsaa < 0)
            return;
        // Only in the game: another host (the proxy test) has nothing here.
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery((void*)0x00EC3730, &info, sizeof(info)) ||
            info.State != MEM_COMMIT ||
            !(info.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                              PAGE_EXECUTE_WRITECOPY)))
            return;
        // sub esp, 5Ch / push ebp / push esi / push edi
        const unsigned char sig[] = { 0x83, 0xEC, 0x5C, 0x55, 0x56, 0x57 };
        void* tramp = nullptr;
        if (hook_install((void*)0x00EC3730,
                         (void*)&detour_load_graphics_settings, &tramp,
                         sig, sizeof(sig),
                         "graphics settings loader (depth_of_field)"))
            g_load_graphics_settings =
                reinterpret_cast<PFN_LoadGraphicsSettings>(tramp);
        // Already loaded by now: set the live block before the device copies
        // it (PCDeviceManager init, 0x00415C36).
        MEMORY_BASIC_INFORMATION block{};
        if (VirtualQuery((void*)0x00F12008, &block, sizeof(block)) &&
            block.State == MEM_COMMIT &&
            (block.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE |
                              PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)))
            override_settings((void*)0x00F12008, "at DLL load");
    }

    void hook_apply_gog_crash_guard()
    {
        struct Patch
        {
            unsigned char* at;
            unsigned char retail[12];
            unsigned char guarded[12];
            size_t size;
        };
        // Byte for byte from GOG's trl.exe (same build, 0x446CAB36).
        const Patch patches[3] = {
            // New stub in padding: add eax, 0x0113F880; jmp 0x005C136B
            { reinterpret_cast<unsigned char*>(0x005C1222),
              { 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC },
              { 0x05, 0x80, 0xF8, 0x13, 0x01, 0xE9, 0x3F, 0x01, 0x00, 0x00 },
              10 },
            // add eax, 0x0113F880 -> jmp 0x005C1534 (the check)
            { reinterpret_cast<unsigned char*>(0x005C1366),
              { 0x05, 0x80, 0xF8, 0x13, 0x01 },
              { 0xE9, 0xC9, 0x01, 0x00, 0x00 },
              5 },
            // Check in padding: cmp eax, 0x01000000; jg 0x005C1505 (skip
            // the node); jmp 0x005C1222
            { reinterpret_cast<unsigned char*>(0x005C1534),
              { 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
                0xCC, 0xCC },
              { 0x3D, 0x00, 0x00, 0x00, 0x01, 0x7F, 0xCA, 0xE9, 0xE2, 0xFC,
                0xFF, 0xFF },
              12 },
        };
        bool all_retail = true, all_guarded = true;
        for (const Patch& p : patches)
        {
            all_retail = all_retail && memcmp(p.at, p.retail, p.size) == 0;
            all_guarded = all_guarded && memcmp(p.at, p.guarded, p.size) == 0;
        }
        if (all_guarded)
        {
            log("hook: GOG terrain crash guard already present (GOG exe)");
            return;
        }
        if (!all_retail)
        {
            log("hook: GOG terrain crash guard not applied -- bytes differ "
                "from the known retail build");
            return;
        }
        for (const Patch& p : patches)
        {
            DWORD old = 0;
            if (!VirtualProtect(p.at, p.size, PAGE_EXECUTE_READWRITE, &old))
            {
                log("hook: GOG terrain crash guard -- could not unprotect %p",
                    p.at);
                return;
            }
            memcpy(p.at, p.guarded, p.size);
            VirtualProtect(p.at, p.size, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p.at, p.size);
        }
        log("hook: GOG terrain crash guard applied (0x005C1366 index check)");
    }
}
