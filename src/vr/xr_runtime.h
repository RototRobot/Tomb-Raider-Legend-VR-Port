#pragma once
// Everything that talks to OpenXR: instance, session, frame loop, swapchain
// and the controller actions. The rest of the mod sees poses as 3x4 matrices
// and frusta as tangents, and never includes openxr.h.
//
// The loader is linked statically, so there is no DLL of ours to go missing;
// a missing or broken runtime fails xrCreateInstance, which is logged, and
// the game runs flat.

#include <cstdint>

namespace trlvr
{
    // Device -> tracking space, row-major 3x4: the rotation's columns are the
    // device's local axes, the last column its position in metres. Right
    // handed, +Y up, -Z forward.
    struct Mat34
    {
        float m[3][4];
    };

    struct Vec3
    {
        float v[3];
    };

    struct TrackedPose
    {
        Mat34 mDeviceToAbsoluteTracking;
        Vec3 vVelocity;
        Vec3 vAngularVelocity;
        bool bPoseIsValid;
        bool bDeviceIsConnected;
    };

    // The poses the mod tracks. The hands are the controllers' grip poses.
    enum : unsigned { kPoseHmd = 0, kPoseLeft = 1, kPoseRight = 2, kPoseCount = 3 };

    // One eye as the runtime reports it for the frame being rendered.
    struct XrEyeView
    {
        // Half-angle tangents in the layout EyeInfo has always held: left
        // negative, right positive, and "top" the down edge (negative) with
        // "bottom" the up edge (positive).
        float tan_left, tan_right, tan_top, tan_bottom;
        Mat34 eye_to_head;
    };

    // --- instance ---------------------------------------------------------
    // Instance and system only; no graphics yet. Also publishes the Vulkan
    // extensions the runtime needs, for the DXVK fork to pick up when it
    // creates its instance and device.
    bool xr_instance_init();
    void xr_instance_shutdown();
    bool xr_instance_ready();
    const char* xr_last_error();
    const char* xr_system_name();
    bool xr_recommended_size(unsigned* width, unsigned* height);

    // --- session ----------------------------------------------------------
    struct XrVulkanHandles
    {
        void* instance;
        void* physical_device;
        void* device;
        uint32_t queue_family;
        uint32_t queue_index;
    };

    // The runtime may use the Vulkan queue inside these, so the caller holds
    // DXVK's submission queue across them.
    bool xr_session_create(const XrVulkanHandles& vk);
    void xr_session_destroy();
    bool xr_session_exists();

    // --- frame ------------------------------------------------------------
    struct XrFrame
    {
        TrackedPose poses[kPoseCount];
        XrEyeView eyes[2];
        bool eyes_valid;
    };

    // Polls events, then xrWaitFrame + xrBeginFrame and the poses predicted
    // for that frame. False when no frame was begun (session not running).
    // Does not touch the Vulkan queue.
    bool xr_frame_wait(XrFrame* out);
    bool xr_frame_begun();
    // False while the runtime wants frames kept in step but not drawn.
    bool xr_frame_should_render();
    // Milliseconds xrWaitFrame blocked for the frame last begun.
    double xr_frame_wait_ms();

    // The swapchain is created to fit what is submitted: one image as wide
    // as both eyes side by side. These use the queue; hold the lock.
    bool xr_swapchain_matches(unsigned width, unsigned height);
    bool xr_swapchain_create(unsigned width, unsigned height);
    void xr_swapchain_destroy();
    // VkFormat of the swapchain, and whether it is BGRA like the back buffer.
    int xr_swapchain_format();
    bool xr_swapchain_acquire(uint64_t* vk_image);
    void xr_swapchain_release();

    struct XrRect
    {
        int x, y, width, height;
    };
    // Ends the frame begun by xr_frame_wait with one projection layer, each
    // eye a rectangle of the swapchain image; null rectangles end it with no
    // layer. The poses are the ones xr_frame_wait located.
    bool xr_frame_end(const XrRect* left, const XrRect* right);

    // --- controllers --------------------------------------------------------
    // Physical inputs, per hand (0 left, 1 right). Which game action each
    // one produces is decided in vr_input.cpp.
    enum XrButton
    {
        XrButtonStick,      // thumbstick click
        XrButtonPrimary,    // A / X (Index: A)
        XrButtonSecondary,  // B / Y (Index: B)
        XrButtonTrackpad,   // Index and WMR
        XrButtonMenu,       // WMR menu, left Touch menu
        // Capacitive sensors (Touch, Index): a finger resting on them.
        XrTouchTrigger,     // index finger on the trigger
        XrTouchThumb,       // thumb on the stick, a face button or the rest
        XrButtonCount
    };

    enum XrControllerKind
    {
        XrControllerNone,
        XrControllerTouch,
        XrControllerIndex,
        XrControllerWmr,
        XrControllerHpReverb,
        XrControllerSimple,
    };

    // True once the actions were synced this frame (session focused).
    bool xr_input_active();
    bool xr_input_button(int hand, XrButton button);
    float xr_input_trigger(int hand);
    float xr_input_squeeze(int hand);
    void xr_input_stick(int hand, float* x, float* y);
    bool xr_input_haptic(int hand, float seconds, float frequency, float amplitude);
    XrControllerKind xr_input_controller(int hand);
    // The interaction profile path for that hand, "" until one is bound.
    const char* xr_input_profile(int hand);
}
