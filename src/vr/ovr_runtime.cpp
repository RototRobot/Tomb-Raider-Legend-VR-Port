#include "vr_backend.h"

#include "../common/log.h"

#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <openvr.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// OpenVR / SteamVR behind xr_runtime.h (1.1.0): the fallback backend, built
// from the 1.0.0 code that shipped (vr_session.cpp init, vr_submit.cpp
// submit). vr_input.cpp decides game actions from physical controls, so
// SteamVR's action manifest here is one action per physical control and
// hand (trlvr_actions.json + bindings_*.json beside the game); players can
// still rebind them in SteamVR's bindings screen.
//
// openvr_api.dll is loaded at run time, beside the game first.

namespace trlvr
{
    namespace
    {
        typedef uint32_t(__cdecl* PFN_InitInternal2)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
        typedef void(__cdecl* PFN_ShutdownInternal)();
        typedef void* (__cdecl* PFN_GetGenericInterface)(const char*, vr::EVRInitError*);
        typedef bool(__cdecl* PFN_IsHmdPresent)();
        typedef const char* (__cdecl* PFN_GetInitErrorAsEnglishDescription)(vr::EVRInitError);

        HMODULE g_dll = nullptr;
        PFN_InitInternal2 p_InitInternal2 = nullptr;
        PFN_ShutdownInternal p_ShutdownInternal = nullptr;
        PFN_GetGenericInterface p_GetGenericInterface = nullptr;
        PFN_IsHmdPresent p_IsHmdPresent = nullptr;
        PFN_GetInitErrorAsEnglishDescription p_ErrorText = nullptr;

        vr::IVRSystem* g_system = nullptr;
        vr::IVRCompositor* g_comp = nullptr;
        vr::IVRInput* g_input = nullptr;
        bool g_initialised = false;   // VR_InitInternal2 succeeded
        bool g_session = false;
        bool g_frame_begun = false;
        bool g_input_active = false;
        double g_wait_ms = 0.0;
        char g_error[512] = "";
        char g_system_name[256] = "";

        // One action per physical control and hand.
        vr::VRActionSetHandle_t g_set = vr::k_ulInvalidActionSetHandle;
        vr::VRActionHandle_t g_stick[2]{}, g_trigger[2]{}, g_squeeze[2]{};
        vr::VRActionHandle_t g_button[XrButtonCount][2]{};
        vr::VRActionHandle_t g_pose[2]{}, g_haptic[2]{};
        // SteamVR's OpenXR grip pose ("openxr_grip"), read only to log how
        // far it is from the OpenVR grip pose the hands use (user
        // 2026-10-27: OpenXR's hands sat a little forward and higher).
        vr::VRActionHandle_t g_pose_xr[2]{};
        ULONGLONG g_pose_compare_at = 0;
        int g_pose_compare_reports = 0;
        vr::TrackedDevicePose_t g_device_poses[vr::k_unMaxTrackedDeviceCount]{};

        XrControllerKind g_controller[2] = { XrControllerNone, XrControllerNone };
        char g_profile[2][96] = { "", "" };
        char g_type[2][64] = { "", "" };
        ULONGLONG g_profile_checked = 0;

        void set_error(const char* fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            vsnprintf(g_error, sizeof(g_error), fmt, args);
            va_end(args);
        }

        bool load_dll()
        {
            if (g_dll)
                return true;
            wchar_t path[MAX_PATH]{};
            swprintf_s(path, L"%sopenvr_api.dll", exe_dir());
            g_dll = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!g_dll)
                g_dll = LoadLibraryW(L"openvr_api.dll");
            if (!g_dll)
            {
                set_error("openvr_api.dll not found beside the game or on the path");
                return false;
            }
            p_InitInternal2 = (PFN_InitInternal2)GetProcAddress(g_dll, "VR_InitInternal2");
            p_ShutdownInternal = (PFN_ShutdownInternal)GetProcAddress(g_dll, "VR_ShutdownInternal");
            p_GetGenericInterface = (PFN_GetGenericInterface)GetProcAddress(g_dll, "VR_GetGenericInterface");
            p_IsHmdPresent = (PFN_IsHmdPresent)GetProcAddress(g_dll, "VR_IsHmdPresent");
            p_ErrorText = (PFN_GetInitErrorAsEnglishDescription)GetProcAddress(g_dll, "VR_GetVRInitErrorAsEnglishDescription");
            if (!p_InitInternal2 || !p_GetGenericInterface || !p_ShutdownInternal)
            {
                set_error("openvr_api.dll is missing the entry points we need");
                return false;
            }
            return true;
        }

        void* get_interface(const char* version)
        {
            vr::EVRInitError err = vr::VRInitError_None;
            void* p = p_GetGenericInterface(version, &err);
            return err == vr::VRInitError_None ? p : nullptr;
        }

        // DXVK reads the Vulkan extensions the runtime needs from the
        // environment before it creates its instance (dxvk_openxr.cpp, the
        // same variables the OpenXR backend fills). OpenVR names the device
        // extensions only for a physical device, so a throwaway instance is
        // made to ask which one the headset is on.
        bool publish_vulkan_extensions()
        {
            char instance_exts[4096]{};
            g_comp->GetVulkanInstanceExtensionsRequired(instance_exts,
                                                        sizeof(instance_exts));

            std::string device_exts;
            HMODULE vk = LoadLibraryW(L"vulkan-1.dll");
            auto get_proc = vk ? (PFN_vkGetInstanceProcAddr)GetProcAddress(
                                     vk, "vkGetInstanceProcAddr")
                               : nullptr;
            auto create = get_proc ? (PFN_vkCreateInstance)get_proc(
                                         nullptr, "vkCreateInstance")
                                   : nullptr;
            if (create)
            {
                std::string list = instance_exts;
                std::vector<std::string> names;
                for (size_t a = 0; a < list.size();)
                {
                    size_t b = list.find(' ', a);
                    if (b == std::string::npos) b = list.size();
                    if (b > a) names.push_back(list.substr(a, b - a));
                    a = b + 1;
                }
                std::vector<const char*> ptrs;
                for (const std::string& n : names)
                    ptrs.push_back(n.c_str());
                VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
                app.apiVersion = VK_API_VERSION_1_1;
                VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
                info.pApplicationInfo = &app;
                info.enabledExtensionCount = (uint32_t)ptrs.size();
                info.ppEnabledExtensionNames = ptrs.data();
                VkInstance instance = VK_NULL_HANDLE;
                if (create(&info, nullptr, &instance) == VK_SUCCESS)
                {
                    uint64_t physical = 0;
                    g_system->GetOutputDevice(&physical, vr::TextureType_Vulkan,
                                              (VkInstance_T*)instance);
                    if (physical)
                    {
                        char buffer[4096]{};
                        g_comp->GetVulkanDeviceExtensionsRequired(
                            (VkPhysicalDevice_T*)(uintptr_t)physical, buffer,
                            sizeof(buffer));
                        device_exts = buffer;
                    }
                    auto destroy = (PFN_vkDestroyInstance)get_proc(
                        instance, "vkDestroyInstance");
                    if (destroy)
                        destroy(instance, nullptr);
                }
            }
            if (device_exts.empty())
            {
                // What SteamVR asks for on every driver we know of.
                device_exts = "VK_KHR_external_memory VK_KHR_external_memory_win32 "
                              "VK_KHR_external_semaphore VK_KHR_external_semaphore_win32 "
                              "VK_KHR_dedicated_allocation VK_KHR_get_memory_requirements2";
                log("openvr: could not ask for the device extensions; using "
                    "the usual SteamVR list");
            }
            SetEnvironmentVariableA("TRLVR_XR_VK_INSTANCE_EXTENSIONS", instance_exts);
            SetEnvironmentVariableA("TRLVR_XR_VK_DEVICE_EXTENSIONS", device_exts.c_str());
            log("openvr: Vulkan instance extensions: %s", instance_exts);
            log("openvr: Vulkan device extensions: %s", device_exts.c_str());
            return true;
        }

        bool setup_actions()
        {
            g_input = (vr::IVRInput*)get_interface(vr::IVRInput_Version);
            if (!g_input)
            {
                log("openvr: no %s -- no controller input", vr::IVRInput_Version);
                return false;
            }
            wchar_t wide[MAX_PATH]{};
            swprintf_s(wide, L"%strlvr_actions.json", exe_dir());
            char path[MAX_PATH * 3]{};
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, (int)sizeof(path),
                                nullptr, nullptr);
            const vr::EVRInputError merr = g_input->SetActionManifestPath(path);
            if (merr != vr::VRInputError_None)
            {
                log("openvr: SetActionManifestPath(%s) failed, EVRInputError %d "
                    "-- is trlvr_actions.json beside the game?", path, (int)merr);
                return false;
            }
            g_input->GetActionSetHandle("/actions/trlvr", &g_set);
            static const char* const kSide[2] = { "left", "right" };
            static const char* const kButton[XrButtonCount] = {
                "stick_click", "primary", "secondary", "trackpad", "menu",
                "trigger_touch", "thumb_touch" };
            char name[128];
            for (int h = 0; h < 2; ++h)
            {
                sprintf_s(name, "/actions/trlvr/in/stick_%s", kSide[h]);
                g_input->GetActionHandle(name, &g_stick[h]);
                sprintf_s(name, "/actions/trlvr/in/trigger_%s", kSide[h]);
                g_input->GetActionHandle(name, &g_trigger[h]);
                sprintf_s(name, "/actions/trlvr/in/squeeze_%s", kSide[h]);
                g_input->GetActionHandle(name, &g_squeeze[h]);
                for (int b = 0; b < XrButtonCount; ++b)
                {
                    sprintf_s(name, "/actions/trlvr/in/%s_%s", kButton[b], kSide[h]);
                    g_input->GetActionHandle(name, &g_button[b][h]);
                }
                sprintf_s(name, "/actions/trlvr/in/hand_%s", kSide[h]);
                g_input->GetActionHandle(name, &g_pose[h]);
                sprintf_s(name, "/actions/trlvr/in/hand_xr_%s", kSide[h]);
                g_input->GetActionHandle(name, &g_pose_xr[h]);
                sprintf_s(name, "/actions/trlvr/out/haptic_%s", kSide[h]);
                g_input->GetActionHandle(name, &g_haptic[h]);
            }
            log("openvr: controller actions from %s", path);
            return true;
        }

        // The controller type SteamVR reports, as the OpenXR profile the
        // rest of the mod already knows (tuning profiles, bindings logic).
        void update_profiles()
        {
            const ULONGLONG now = GetTickCount64();
            if (now - g_profile_checked < 1000)
                return;
            g_profile_checked = now;
            for (int h = 0; h < 2; ++h)
            {
                const vr::TrackedDeviceIndex_t index =
                    g_system->GetTrackedDeviceIndexForControllerRole(
                        h == 0 ? vr::TrackedControllerRole_LeftHand
                               : vr::TrackedControllerRole_RightHand);
                char type[64]{};
                if (index != vr::k_unTrackedDeviceIndexInvalid)
                    g_system->GetStringTrackedDeviceProperty(index,
                        vr::Prop_ControllerType_String, type, sizeof(type));
                if (strcmp(type, g_type[h]) == 0)
                    continue;
                strcpy_s(g_type[h], type);
                const char* profile = "";
                XrControllerKind kind = XrControllerNone;
                if (!type[0])
                    ;
                else if (strstr(type, "oculus_touch"))
                {
                    kind = XrControllerTouch;
                    profile = "/interaction_profiles/oculus/touch_controller";
                }
                else if (strstr(type, "knuckles"))
                {
                    kind = XrControllerIndex;
                    profile = "/interaction_profiles/valve/index_controller";
                }
                else if (strstr(type, "hpmotioncontroller"))
                {
                    kind = XrControllerHpReverb;
                    profile = "/interaction_profiles/hp/mixed_reality_controller";
                }
                else if (strstr(type, "holographic"))
                {
                    kind = XrControllerWmr;
                    profile = "/interaction_profiles/microsoft/motion_controller";
                }
                else
                {
                    kind = XrControllerSimple;
                    profile = "/interaction_profiles/khr/simple_controller";
                }
                g_controller[h] = kind;
                strcpy_s(g_profile[h], profile);
                log("openvr: %s hand controller type \"%s\" (%s)",
                    h == 0 ? "left" : "right", type[0] ? type : "none",
                    profile[0] ? profile : "-");
            }
        }

        bool digital(vr::VRActionHandle_t h)
        {
            if (!g_input_active || !h)
                return false;
            vr::InputDigitalActionData_t d{};
            return g_input->GetDigitalActionData(h, &d, sizeof(d),
                       vr::k_ulInvalidInputValueHandle) ==
                       vr::VRInputError_None && d.bActive && d.bState;
        }

        bool analog(vr::VRActionHandle_t h, float* x, float* y)
        {
            if (!g_input_active || !h)
                return false;
            vr::InputAnalogActionData_t a{};
            if (g_input->GetAnalogActionData(h, &a, sizeof(a),
                    vr::k_ulInvalidInputValueHandle) != vr::VRInputError_None ||
                !a.bActive)
                return false;
            if (x) *x = a.x;
            if (y) *y = a.y;
            return true;
        }

        // Every 10 s (12 reports): where SteamVR's OpenXR grip pose sits in
        // the OpenVR grip pose's own frame -- the offset between the two
        // backends' hands -- in cm (x right, y up, z back) and degrees.
        void compare_poses()
        {
            const ULONGLONG now = GetTickCount64();
            if (g_pose_compare_reports >= 12 || now - g_pose_compare_at < 10000)
                return;
            g_pose_compare_at = now;
            for (int h = 0; h < 2; ++h)
            {
                vr::InputPoseActionData_t a{}, b{};
                const bool have_a = g_pose[h] &&
                    g_input->GetPoseActionDataForNextFrame(g_pose[h],
                        vr::TrackingUniverseSeated, &a, sizeof(a),
                        vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None &&
                    a.bActive && a.pose.bPoseIsValid;
                const bool have_b = g_pose_xr[h] &&
                    g_input->GetPoseActionDataForNextFrame(g_pose_xr[h],
                        vr::TrackingUniverseSeated, &b, sizeof(b),
                        vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None &&
                    b.bActive && b.pose.bPoseIsValid;
                if (!have_a)
                    continue;
                if (!have_b)
                {
                    if (g_pose_compare_reports == 0)
                        log("openvr: %s hand: SteamVR gives no openxr_grip pose "
                            "for this controller -- no comparison",
                            h == 0 ? "left" : "right");
                    continue;
                }
                const vr::HmdMatrix34_t& m = a.pose.mDeviceToAbsoluteTracking;
                const vr::HmdMatrix34_t& n = b.pose.mDeviceToAbsoluteTracking;
                float d[3], local[3];
                for (int k = 0; k < 3; ++k)
                    d[k] = n.m[k][3] - m.m[k][3];
                // Into the grip frame: the rotation's columns are its axes.
                for (int c = 0; c < 3; ++c)
                    local[c] = m.m[0][c] * d[0] + m.m[1][c] * d[1] + m.m[2][c] * d[2];
                // Angle between the two rotations: trace of A^T B.
                float trace = 0.0f;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        trace += m.m[r][c] * n.m[r][c];
                const float cosine = fmaxf(-1.0f, fminf(1.0f, (trace - 1.0f) * 0.5f));
                const float angle = acosf(cosine);
                // The turn as a rotation vector in the OpenVR grip's frame:
                // R = A^T B, axis from its antisymmetric part.
                float rel[3][3];
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        rel[r][c] = m.m[0][r] * n.m[0][c] + m.m[1][r] * n.m[1][c] +
                                    m.m[2][r] * n.m[2][c];
                float axis[3] = { rel[2][1] - rel[1][2], rel[0][2] - rel[2][0],
                                  rel[1][0] - rel[0][1] };
                const float len = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] +
                                        axis[2] * axis[2]);
                for (float& v : axis)
                    v = len > 1e-6f ? v / len * angle * 57.29578f : 0.0f;
                log("openvr: %s hand: OpenXR grip pose sits (%+.2f, %+.2f, %+.2f) cm "
                    "from the OpenVR grip pose (x right, y up, z back), turned "
                    "%.2f deg about (%+.2f, %+.2f, %+.2f) deg-vector; rotation rows "
                    "(%+.4f %+.4f %+.4f / %+.4f %+.4f %+.4f / %+.4f %+.4f %+.4f)",
                    h == 0 ? "left" : "right", local[0] * 100.0f,
                    local[1] * 100.0f, local[2] * 100.0f, angle * 57.29578f,
                    axis[0], axis[1], axis[2], rel[0][0], rel[0][1], rel[0][2],
                    rel[1][0], rel[1][1], rel[1][2], rel[2][0], rel[2][1], rel[2][2]);
            }
            ++g_pose_compare_reports;
        }

        void copy_pose(const vr::TrackedDevicePose_t& in, TrackedPose* out)
        {
            *out = TrackedPose{};
            out->bDeviceIsConnected = in.bDeviceIsConnected;
            out->bPoseIsValid = in.bPoseIsValid;
            if (!in.bPoseIsValid)
                return;
            memcpy(out->mDeviceToAbsoluteTracking.m, in.mDeviceToAbsoluteTracking.m,
                   sizeof(float) * 12);
            memcpy(out->vVelocity.v, in.vVelocity.v, sizeof(float) * 3);
            memcpy(out->vAngularVelocity.v, in.vAngularVelocity.v, sizeof(float) * 3);
        }
    }

    // --- instance -------------------------------------------------------------

    bool ovr_instance_ready() { return g_initialised && g_system && g_comp; }
    const char* ovr_last_error() { return g_error; }
    const char* ovr_system_name() { return g_system_name; }

    bool ovr_instance_init()
    {
        if (ovr_instance_ready())
            return true;
        if (!load_dll())
            return false;
        if (p_IsHmdPresent && !p_IsHmdPresent())
        {
            set_error("no headset detected");
            return false;
        }
        vr::EVRInitError err = vr::VRInitError_None;
        p_InitInternal2(&err, vr::VRApplication_Scene, nullptr);
        if (err != vr::VRInitError_None)
        {
            set_error("VR_Init failed: %s",
                      p_ErrorText ? p_ErrorText(err) : "unknown error");
            return false;
        }
        g_initialised = true;
        g_system = (vr::IVRSystem*)get_interface(vr::IVRSystem_Version);
        g_comp = (vr::IVRCompositor*)get_interface(vr::IVRCompositor_Version);
        if (!g_system || !g_comp)
        {
            set_error("could not get %s / %s", vr::IVRSystem_Version,
                      vr::IVRCompositor_Version);
            return false;
        }
        // Seated: the origin is where the head was at the last reset, as
        // OpenXR's LOCAL space.
        g_comp->SetTrackingSpace(vr::TrackingUniverseSeated);

        char model[128]{}, number[128]{};
        g_system->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
            vr::Prop_TrackingSystemName_String, model, sizeof(model));
        g_system->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
            vr::Prop_ModelNumber_String, number, sizeof(number));
        sprintf_s(g_system_name, "%s / %s", model, number);

        publish_vulkan_extensions();
        setup_actions();
        return true;
    }

    void ovr_instance_shutdown()
    {
        ovr_session_destroy();
        if (g_initialised && p_ShutdownInternal)
            p_ShutdownInternal();
        g_initialised = false;
        g_system = nullptr;
        g_comp = nullptr;
        g_input = nullptr;
        g_input_active = false;
    }

    bool ovr_recommended_size(unsigned* width, unsigned* height)
    {
        if (!ovr_instance_ready())
            return false;
        uint32_t w = 0, h = 0;
        g_system->GetRecommendedRenderTargetSize(&w, &h);
        if (width) *width = w;
        if (height) *height = h;
        return w && h;
    }

    // --- session --------------------------------------------------------------

    bool ovr_session_create(const XrVulkanHandles&)
    {
        g_session = ovr_instance_ready();
        if (g_session)
            log("openvr: session ready (frames go to the SteamVR compositor)");
        return g_session;
    }

    void ovr_session_destroy()
    {
        g_session = false;
        g_frame_begun = false;
    }

    bool ovr_session_exists() { return g_session; }

    // --- frame ----------------------------------------------------------------

    bool ovr_frame_wait(XrFrame* out)
    {
        if (!g_session || g_frame_begun)
            return false;

        vr::VREvent_t event{};
        while (g_system->PollNextEvent(&event, sizeof(event)))
        {
            if (event.eventType == vr::VREvent_Quit)
            {
                log("openvr: SteamVR asked the game to quit; acknowledged");
                g_system->AcknowledgeQuit_Exiting();
            }
        }

        LARGE_INTEGER frequency{}, before{}, after{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&before);
        log_set_activity("IVRCompositor::WaitGetPoses");
        const bool posed = g_comp->WaitGetPoses(g_device_poses,
            vr::k_unMaxTrackedDeviceCount, nullptr, 0) == vr::VRCompositorError_None;
        log_set_activity(nullptr);
        QueryPerformanceCounter(&after);
        g_wait_ms = double(after.QuadPart - before.QuadPart) * 1000.0 /
                    double(frequency.QuadPart);
        if (!posed)
            return false;
        g_frame_begun = true;

        g_input_active = false;
        if (g_input && g_set != vr::k_ulInvalidActionSetHandle)
        {
            vr::VRActiveActionSet_t active{};
            active.ulActionSet = g_set;
            log_set_activity("IVRInput::UpdateActionState");
            g_input_active = g_input->UpdateActionState(&active, sizeof(active), 1) ==
                             vr::VRInputError_None;
            log_set_activity(nullptr);
        }
        update_profiles();
        if (g_input_active)
            compare_poses();

        if (!out)
            return true;
        copy_pose(g_device_poses[vr::k_unTrackedDeviceIndex_Hmd],
                  &out->poses[kPoseHmd]);
        for (int h = 0; h < 2; ++h)
        {
            out->poses[kPoseLeft + h] = TrackedPose{};
            vr::InputPoseActionData_t pose{};
            if (g_input_active && g_pose[h] &&
                g_input->GetPoseActionDataForNextFrame(g_pose[h],
                    vr::TrackingUniverseSeated, &pose, sizeof(pose),
                    vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None &&
                pose.bActive)
                copy_pose(pose.pose, &out->poses[kPoseLeft + h]);
        }
        for (int i = 0; i < 2; ++i)
        {
            const vr::EVREye e = i == 0 ? vr::Eye_Left : vr::Eye_Right;
            XrEyeView& eye = out->eyes[i];
            // GetProjectionRaw is the layout EyeInfo has always held.
            g_system->GetProjectionRaw(e, &eye.tan_left, &eye.tan_right,
                                       &eye.tan_top, &eye.tan_bottom);
            const vr::HmdMatrix34_t t = g_system->GetEyeToHeadTransform(e);
            memcpy(eye.eye_to_head.m, t.m, sizeof(float) * 12);
        }
        out->eyes_valid = true;
        return true;
    }

    bool ovr_frame_begun() { return g_frame_begun; }
    bool ovr_frame_should_render() { return g_frame_begun; }
    double ovr_frame_wait_ms() { return g_wait_ms; }

    // No swapchain: vr_submit hands the surface over in place (ovr_submit).
    bool ovr_swapchain_matches(unsigned, unsigned) { return true; }
    bool ovr_swapchain_create(unsigned, unsigned) { return true; }
    void ovr_swapchain_destroy() {}
    int ovr_swapchain_format() { return 0; }
    bool ovr_swapchain_acquire(uint64_t*) { return false; }
    void ovr_swapchain_release() {}

    bool ovr_frame_end(const XrRect*, const XrRect*)
    {
        if (!g_frame_begun)
            return false;
        g_frame_begun = false;
        if (g_comp)
            g_comp->PostPresentHandoff();
        return true;
    }

    bool ovr_submit(const OvrImage& left, const float left_bounds[4],
                    const OvrImage& right, const float right_bounds[4])
    {
        if (!g_comp)
            return false;
        const OvrImage* images[2] = { &left, &right };
        const float* bounds[2] = { left_bounds, right_bounds };
        bool ok = true;
        for (int i = 0; i < 2 && ok; ++i)
        {
            const OvrImage& img = *images[i];
            vr::VRVulkanTextureData_t vk{};
            vk.m_nImage = img.image;
            vk.m_pDevice = (VkDevice_T*)img.device;
            vk.m_pPhysicalDevice = (VkPhysicalDevice_T*)img.physical_device;
            vk.m_pInstance = (VkInstance_T*)img.instance;
            vk.m_pQueue = (VkQueue_T*)img.queue;
            vk.m_nQueueFamilyIndex = img.queue_family;
            vk.m_nWidth = img.width;
            vk.m_nHeight = img.height;
            vk.m_nFormat = img.format;
            vk.m_nSampleCount = img.samples;
            vr::Texture_t tex{};
            tex.handle = &vk;
            tex.eType = vr::TextureType_Vulkan;
            tex.eColorSpace = vr::ColorSpace_Auto;
            vr::VRTextureBounds_t b{ bounds[i][0], bounds[i][1],
                                     bounds[i][2], bounds[i][3] };
            log_set_activity(i == 0 ? "IVRCompositor::Submit (left eye)"
                                    : "IVRCompositor::Submit (right eye)");
            const vr::EVRCompositorError err = g_comp->Submit(
                i == 0 ? vr::Eye_Left : vr::Eye_Right, &tex, &b,
                vr::Submit_Default);
            log_set_activity(nullptr);
            if (err != vr::VRCompositorError_None)
            {
                static int reports = 0;
                if (reports < 5)
                {
                    ++reports;
                    log("openvr: Submit (%s eye) failed, EVRCompositorError %d",
                        i == 0 ? "left" : "right", (int)err);
                }
                ok = false;
            }
        }
        return ok;
    }

    // --- controllers ------------------------------------------------------------

    bool ovr_input_active() { return g_input_active; }

    bool ovr_input_button(int hand, XrButton button)
    {
        if (hand < 0 || hand > 1 || button < 0 || button >= XrButtonCount)
            return false;
        return digital(g_button[button][hand]);
    }

    float ovr_input_trigger(int hand)
    {
        float x = 0.0f;
        return hand >= 0 && hand <= 1 && analog(g_trigger[hand], &x, nullptr)
            ? x : 0.0f;
    }

    float ovr_input_squeeze(int hand)
    {
        return hand >= 0 && hand <= 1 && digital(g_squeeze[hand]) ? 1.0f : 0.0f;
    }

    void ovr_input_stick(int hand, float* x, float* y)
    {
        *x = *y = 0.0f;
        if (hand >= 0 && hand <= 1)
            analog(g_stick[hand], x, y);
    }

    bool ovr_input_haptic(int hand, float seconds, float frequency, float amplitude)
    {
        if (!g_input_active || hand < 0 || hand > 1 || !g_haptic[hand])
            return false;
        amplitude = amplitude < 0.0f ? 0.0f : amplitude > 1.0f ? 1.0f : amplitude;
        return g_input->TriggerHapticVibrationAction(g_haptic[hand], 0.0f,
                   seconds, frequency, amplitude,
                   vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None;
    }

    XrControllerKind ovr_input_controller(int hand)
    {
        return hand < 0 || hand > 1 ? XrControllerNone : g_controller[hand];
    }

    const char* ovr_input_profile(int hand)
    {
        return hand < 0 || hand > 1 ? "" : g_profile[hand];
    }
}
