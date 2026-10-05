#include "water_stereo.h"

#include "hook.h"

#include "../common/log.h"

#include <cstdint>
#include <cstring>

namespace trlvr
{
    namespace
    {
        // PCWaterEffect::EndScene(PCRenderContext*) in the retail Steam exe.
        // This pass renders the ordinary half-resolution water mask. Marking
        // it remains functional: the stereo layer splits that target into two
        // eyes instead of treating it as an unrelated post effect.
        const uintptr_t kWaterEndScene = 0x0061AF30;

        typedef void (__thiscall* WaterEndSceneFn)(void*, void*);
        WaterEndSceneFn g_original = nullptr;
        int g_depth = 0;

        struct BranchPatch
        {
            unsigned char* at;
            unsigned char* destination;
            unsigned char expected[6];
        };

        bool write_code(unsigned char* at, const unsigned char* replacement,
                        size_t size)
        {
            DWORD old = 0;
            if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old))
                return false;
            memcpy(at, replacement, size);
            VirtualProtect(at, size, old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, size);
            return true;
        }

        bool force_unconditional_jump(const BranchPatch& patch)
        {
            unsigned char replacement[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
            const intptr_t relative = patch.destination - patch.at - 5;
            memcpy(replacement + 1, &relative, sizeof(int32_t));

            return write_code(patch.at, replacement, sizeof(replacement));
        }

        void disable_mono_planar_reflection()
        {
            // Keep device settings and resource lifetime intact. These are
            // only the render-time gates for the game's separate planar
            // reflection camera, which is also mono in the retail renderer.
            const BranchPatch patches[] = {
                {
                    reinterpret_cast<unsigned char*>(0x0041A2E9),
                    reinterpret_cast<unsigned char*>(0x0041A404),
                    { 0x0F, 0x84, 0x15, 0x01, 0x00, 0x00 }
                },
                {
                    reinterpret_cast<unsigned char*>(0x0041580F),
                    reinterpret_cast<unsigned char*>(0x0041594D),
                    { 0x0F, 0x84, 0x38, 0x01, 0x00, 0x00 }
                }
            };

            unsigned char* nextgen_call =
                reinterpret_cast<unsigned char*>(0x00415A5C);
            const unsigned char nextgen_expected[] =
                { 0xE8, 0xFF, 0x36, 0x00, 0x00 };
            const unsigned char nextgen_nops[] =
                { 0x90, 0x90, 0x90, 0x90, 0x90 };

            if (memcmp(nextgen_call, nextgen_expected,
                       sizeof(nextgen_expected)) != 0)
            {
                log("water stereo: planar reflection left unchanged -- "
                    "executable signature mismatch at 0x%p", nextgen_call);
                return;
            }

            for (const BranchPatch& patch : patches)
            {
                if (memcmp(patch.at, patch.expected,
                           sizeof(patch.expected)) != 0)
                {
                    log("water stereo: planar reflection left unchanged -- "
                        "executable signature mismatch at 0x%p", patch.at);
                    return;
                }
            }

            if (!write_code(nextgen_call, nextgen_nops,
                            sizeof(nextgen_nops)))
            {
                log("water stereo: planar reflection left unchanged -- "
                    "could not suppress next-gen render call at 0x%p",
                    nextgen_call);
                return;
            }

            for (const BranchPatch& patch : patches)
            {
                if (!force_unconditional_jump(patch))
                {
                    log("water stereo: planar reflection left unchanged -- "
                        "could not patch render branch at 0x%p", patch.at);
                    return;
                }
            }

            log("water stereo: mono planar reflection paths suppressed");
        }

        void disable_mono_waterfx_scene_sample()
        {
            // enableWaterFX creates special texture 0x4FE as a full-resolution
            // copy of the current scene. The deep-water renderer later uses
            // that shared texture for refraction/distortion. In stereo the
            // copy contains whichever eye was rendered first, which is why
            // the displaced Lara image changes eyes when free aim changes the
            // camera/eye order.
            //
            // At 0x00446E20 the engine normally skips `mov dl,bl` when
            // WaterFX is enabled. BL is one; leaving the move in place takes
            // the function's existing disabled exit at 0x00446F0F before the
            // mono scene texture is consumed. We deliberately leave the
            // setting, allocation and teardown unchanged to keep level/device
            // transitions consistent.
            unsigned char* branch =
                reinterpret_cast<unsigned char*>(0x00446E20);
            const unsigned char expected[] = { 0x75, 0x02 };
            const unsigned char nops[] = { 0x90, 0x90 };

            if (memcmp(branch, expected, sizeof(expected)) != 0)
            {
                log("water stereo: mono WaterFX scene sample left enabled -- "
                    "executable signature mismatch at 0x%p", branch);
                return;
            }
            if (!write_code(branch, nops, sizeof(nops)))
            {
                log("water stereo: could not disable mono WaterFX scene "
                    "sample at 0x%p", branch);
                return;
            }

            log("water stereo: mono WaterFX scene sample 0x4FE suppressed; "
                "base water rendering retained");
        }

        void __fastcall detour(void* self, void*, void* context)
        {
            ++g_depth;
            g_original(self, context);
            --g_depth;
        }
    }

    void water_stereo_init()
    {
        const uint32_t reflection_route =
            *reinterpret_cast<const uint32_t*>(0x010024E8);
        log("water stereo: active reflection route is %s (flag=%u)",
            reflection_route ? "next-gen" : "previous-gen",
            reflection_route);

        disable_mono_planar_reflection();
        disable_mono_waterfx_scene_sample();

        const unsigned char sig[] = { 0xB8, 0x34, 0x27, 0x00, 0x00 };
        if (hook_install((void*)kWaterEndScene, (void*)&detour,
                         (void**)&g_original, sig, sizeof(sig),
                         "PCWaterEffect::EndScene"))
        {
            log("water stereo: half-resolution water target stereo split "
                "enabled");
        }
    }

    bool water_stereo_active()
    {
        return g_depth > 0;
    }
}
