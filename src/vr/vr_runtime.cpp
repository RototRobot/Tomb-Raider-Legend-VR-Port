#include "vr_backend.h"

#include "../common/config.h"
#include "../common/log.h"


#include <windows.h>

#include <cstdio>

// xr_runtime.h is the one VR interface the mod uses. Since 1.1.0 it has two
// implementations: OpenXR (xr_runtime.cpp, the default) and OpenVR / SteamVR
// (ovr_runtime.cpp, kept as the proven fallback while OpenXR settles in).
// [vr] vr_runtime picks: auto tries OpenXR and falls back to OpenVR when
// openxr_loader.dll is missing beside the game or no OpenXR runtime starts;
// openxr / openvr use only that one.
//
// openxr_loader.dll is delay-loaded (build.bat), so the game starts without
// it: it is loaded here by full path first, and no OpenXR function is called
// unless that worked.

namespace trlvr
{
    namespace
    {
        VrBackend g_backend = VrBackendNone;
        char g_error[512] = "no VR runtime started";

        bool openxr_loader_present()
        {
            wchar_t path[MAX_PATH]{};
            swprintf_s(path, L"%sopenxr_loader.dll", exe_dir());
            if (GetModuleHandleW(L"openxr_loader.dll"))
                return true;
            return LoadLibraryExW(path, nullptr,
                                  LOAD_WITH_ALTERED_SEARCH_PATH) != nullptr;
        }

        bool try_openxr()
        {
            if (!openxr_loader_present())
            {
                sprintf_s(g_error, "openxr_loader.dll not found beside the game");
                log("vr: OpenXR skipped -- %s", g_error);
                return false;
            }
            if (oxr_instance_init())
            {
                g_backend = VrBackendOpenXr;
                log("vr: using OpenXR (%s)", oxr_system_name());
                return true;
            }
            sprintf_s(g_error, "OpenXR: %s", oxr_last_error());
            log("vr: OpenXR did not start -- %s", oxr_last_error());
            oxr_instance_shutdown();
            return false;
        }

        bool try_openvr()
        {
            if (ovr_instance_init())
            {
                g_backend = VrBackendOpenVr;
                log("vr: using OpenVR / SteamVR (%s)", ovr_system_name());
                return true;
            }
            sprintf_s(g_error, "OpenVR: %s", ovr_last_error());
            log("vr: OpenVR did not start -- %s", ovr_last_error());
            ovr_instance_shutdown();
            return false;
        }
    }

    VrBackend vr_backend() { return g_backend; }

#define VR_FORWARD(call, fallback)                                  \
    switch (g_backend)                                              \
    {                                                               \
    case VrBackendOpenXr: return oxr_##call;                        \
    case VrBackendOpenVr: return ovr_##call;                        \
    default: return fallback;                                       \
    }

    bool xr_instance_init()
    {
        if (g_backend != VrBackendNone)
            return true;
        const int choice = config().vr_runtime;
        log("vr: runtime choice %s",
            choice == 1 ? "openxr" : choice == 2 ? "openvr" : "auto");
        if (choice != 2 && try_openxr())
            return true;
        if (choice != 1 && try_openvr())
            return true;
        return false;
    }

    void xr_instance_shutdown()
    {
        if (g_backend == VrBackendOpenXr)
            oxr_instance_shutdown();
        else if (g_backend == VrBackendOpenVr)
            ovr_instance_shutdown();
        g_backend = VrBackendNone;
    }

    bool xr_instance_ready() { VR_FORWARD(instance_ready(), false) }
    const char* xr_last_error() { VR_FORWARD(last_error(), g_error) }
    const char* xr_system_name() { VR_FORWARD(system_name(), "") }
    bool xr_recommended_size(unsigned* width, unsigned* height)
    { VR_FORWARD(recommended_size(width, height), false) }

    bool xr_session_create(const XrVulkanHandles& vk)
    { VR_FORWARD(session_create(vk), false) }
    void xr_session_destroy()
    {
        if (g_backend == VrBackendOpenXr)
            oxr_session_destroy();
        else if (g_backend == VrBackendOpenVr)
            ovr_session_destroy();
    }
    bool xr_session_exists() { VR_FORWARD(session_exists(), false) }

    bool xr_frame_wait(XrFrame* out) { VR_FORWARD(frame_wait(out), false) }
    bool xr_frame_begun() { VR_FORWARD(frame_begun(), false) }
    bool xr_frame_should_render() { VR_FORWARD(frame_should_render(), false) }
    double xr_frame_wait_ms() { VR_FORWARD(frame_wait_ms(), 0.0) }

    bool xr_swapchain_matches(unsigned width, unsigned height)
    { VR_FORWARD(swapchain_matches(width, height), false) }
    bool xr_swapchain_create(unsigned width, unsigned height)
    { VR_FORWARD(swapchain_create(width, height), false) }
    void xr_swapchain_destroy()
    {
        if (g_backend == VrBackendOpenXr)
            oxr_swapchain_destroy();
        else if (g_backend == VrBackendOpenVr)
            ovr_swapchain_destroy();
    }
    int xr_swapchain_format() { VR_FORWARD(swapchain_format(), 0) }
    bool xr_swapchain_acquire(uint64_t* vk_image)
    { VR_FORWARD(swapchain_acquire(vk_image), false) }
    void xr_swapchain_release()
    {
        if (g_backend == VrBackendOpenXr)
            oxr_swapchain_release();
        else if (g_backend == VrBackendOpenVr)
            ovr_swapchain_release();
    }
    bool xr_frame_end(const XrRect* left, const XrRect* right)
    { VR_FORWARD(frame_end(left, right), false) }

    bool xr_input_active() { VR_FORWARD(input_active(), false) }
    bool xr_input_button(int hand, XrButton button)
    { VR_FORWARD(input_button(hand, button), false) }
    float xr_input_trigger(int hand) { VR_FORWARD(input_trigger(hand), 0.0f) }
    float xr_input_squeeze(int hand) { VR_FORWARD(input_squeeze(hand), 0.0f) }
    void xr_input_stick(int hand, float* x, float* y)
    {
        if (g_backend == VrBackendOpenXr)
            oxr_input_stick(hand, x, y);
        else if (g_backend == VrBackendOpenVr)
            ovr_input_stick(hand, x, y);
        else
        {
            if (x) *x = 0.0f;
            if (y) *y = 0.0f;
        }
    }
    bool xr_input_haptic(int hand, float seconds, float frequency, float amplitude)
    { VR_FORWARD(input_haptic(hand, seconds, frequency, amplitude), false) }
    XrControllerKind xr_input_controller(int hand)
    { VR_FORWARD(input_controller(hand), XrControllerNone) }
    const char* xr_input_profile(int hand) { VR_FORWARD(input_profile(hand), "") }

#undef VR_FORWARD
}
