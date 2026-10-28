#pragma once
// The two VR backends behind xr_runtime.h (1.1.0): OpenXR (xr_runtime.cpp,
// oxr_*) and OpenVR / SteamVR (ovr_runtime.cpp, ovr_*). vr_runtime.cpp picks
// one at start-up ([vr] vr_runtime) and forwards every xr_* call to it.

#include "xr_runtime.h"

namespace trlvr
{
    enum VrBackend { VrBackendNone, VrBackendOpenXr, VrBackendOpenVr };
    VrBackend vr_backend();

    // OpenXR
    bool oxr_instance_init();
    void oxr_instance_shutdown();
    bool oxr_instance_ready();
    const char* oxr_last_error();
    const char* oxr_system_name();
    bool oxr_recommended_size(unsigned* width, unsigned* height);
    bool oxr_session_create(const XrVulkanHandles& vk);
    void oxr_session_destroy();
    bool oxr_session_exists();
    bool oxr_frame_wait(XrFrame* out);
    bool oxr_frame_begun();
    bool oxr_frame_should_render();
    double oxr_frame_wait_ms();
    bool oxr_swapchain_matches(unsigned width, unsigned height);
    bool oxr_swapchain_create(unsigned width, unsigned height);
    void oxr_swapchain_destroy();
    int oxr_swapchain_format();
    bool oxr_swapchain_acquire(uint64_t* vk_image);
    void oxr_swapchain_release();
    bool oxr_frame_end(const XrRect* left, const XrRect* right);
    bool oxr_input_active();
    bool oxr_input_button(int hand, XrButton button);
    float oxr_input_trigger(int hand);
    float oxr_input_squeeze(int hand);
    void oxr_input_stick(int hand, float* x, float* y);
    bool oxr_input_haptic(int hand, float seconds, float frequency, float amplitude);
    XrControllerKind oxr_input_controller(int hand);
    const char* oxr_input_profile(int hand);

    // OpenVR
    bool ovr_instance_init();
    void ovr_instance_shutdown();
    bool ovr_instance_ready();
    const char* ovr_last_error();
    const char* ovr_system_name();
    bool ovr_recommended_size(unsigned* width, unsigned* height);
    bool ovr_session_create(const XrVulkanHandles& vk);
    void ovr_session_destroy();
    bool ovr_session_exists();
    bool ovr_frame_wait(XrFrame* out);
    bool ovr_frame_begun();
    bool ovr_frame_should_render();
    double ovr_frame_wait_ms();
    bool ovr_swapchain_matches(unsigned width, unsigned height);
    bool ovr_swapchain_create(unsigned width, unsigned height);
    void ovr_swapchain_destroy();
    int ovr_swapchain_format();
    bool ovr_swapchain_acquire(uint64_t* vk_image);
    void ovr_swapchain_release();
    bool ovr_frame_end(const XrRect* left, const XrRect* right);
    bool ovr_input_active();
    bool ovr_input_button(int hand, XrButton button);
    float ovr_input_trigger(int hand);
    float ovr_input_squeeze(int hand);
    void ovr_input_stick(int hand, float* x, float* y);
    bool ovr_input_haptic(int hand, float seconds, float frequency, float amplitude);
    XrControllerKind ovr_input_controller(int hand);
    const char* ovr_input_profile(int hand);

    // OpenVR has no swapchain: the frame goes to the compositor in place.
    // One Vulkan image (DXVK's description of the surface) and each eye's
    // part of it as u0, v0, u1, v1.
    struct OvrImage
    {
        uint64_t image;
        void* instance;
        void* physical_device;
        void* device;
        void* queue;
        uint32_t queue_family;
        uint32_t width, height, format, samples;
    };
    bool ovr_submit(const OvrImage& left, const float left_bounds[4],
                    const OvrImage& right, const float right_bounds[4]);
}
