#include "xr_runtime.h"

#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace trlvr
{
    namespace
    {
        XrInstance g_instance = XR_NULL_HANDLE;
        XrSystemId g_system = XR_NULL_SYSTEM_ID;
        char g_error[256] = "";
        char g_system_name[XR_MAX_SYSTEM_NAME_SIZE] = "";
        bool g_have_hp_controller = false;

        // XR_KHR_vulkan_enable. Extension functions are not exported by the
        // loader; they come from xrGetInstanceProcAddr.
        PFN_xrGetVulkanGraphicsRequirementsKHR p_GetVulkanGraphicsRequirements = nullptr;
        PFN_xrGetVulkanInstanceExtensionsKHR p_GetVulkanInstanceExtensions = nullptr;
        PFN_xrGetVulkanDeviceExtensionsKHR p_GetVulkanDeviceExtensions = nullptr;
        PFN_xrGetVulkanGraphicsDeviceKHR p_GetVulkanGraphicsDevice = nullptr;

        XrSession g_session = XR_NULL_HANDLE;
        XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
        bool g_running = false;
        XrSpace g_local = XR_NULL_HANDLE;
        XrSpace g_view = XR_NULL_HANDLE;

        bool g_frame_begun = false;
        XrFrameState g_frame{XR_TYPE_FRAME_STATE};
        // The eye poses located for the frame in progress, in LOCAL space:
        // what the game is about to render with, and so what xrEndFrame has
        // to be told or the runtime reprojects from the wrong place.
        XrView g_layer_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        bool g_layer_views_valid = false;
        double g_wait_ms = 0.0;

        XrSwapchain g_swapchain = XR_NULL_HANDLE;
        unsigned g_swapchain_w = 0, g_swapchain_h = 0;
        int64_t g_swapchain_format = 0;
        std::vector<XrSwapchainImageVulkanKHR> g_swapchain_images;
        bool g_swapchain_acquired = false;

        // Controllers.
        XrActionSet g_action_set = XR_NULL_HANDLE;
        XrPath g_hand_path[2] = {XR_NULL_PATH, XR_NULL_PATH};
        XrAction g_act_stick = XR_NULL_HANDLE;
        XrAction g_act_trigger = XR_NULL_HANDLE;
        XrAction g_act_squeeze = XR_NULL_HANDLE;
        XrAction g_act_button[XrButtonCount]{};
        XrAction g_act_grip = XR_NULL_HANDLE;
        XrAction g_act_haptic = XR_NULL_HANDLE;
        XrSpace g_grip_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
        bool g_actions_attached = false;
        bool g_input_active = false;
        bool g_profile_dirty = true;
        XrControllerKind g_controller[2] = {XrControllerNone, XrControllerNone};
        char g_profile[2][XR_MAX_PATH_LENGTH] = {"", ""};

        void set_error(const char* fmt, ...)
        {
            va_list args;
            va_start(args, fmt);
            vsnprintf(g_error, sizeof(g_error), fmt, args);
            va_end(args);
        }

        const char* result_name(XrResult r)
        {
            static char buffer[XR_MAX_RESULT_STRING_SIZE];
            if (g_instance != XR_NULL_HANDLE &&
                XR_SUCCEEDED(xrResultToString(g_instance, r, buffer)))
                return buffer;
            switch (r)
            {
            case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
            case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
            case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
            case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
            case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
            default: break;
            }
            snprintf(buffer, sizeof(buffer), "XrResult %d", (int)r);
            return buffer;
        }

        // Reported once per call site rather than once per frame.
        bool ok(XrResult r, const char* what, bool* reported = nullptr)
        {
            if (XR_SUCCEEDED(r))
                return true;
            if (!reported || !*reported)
                log("xr: %s failed: %s", what, result_name(r));
            if (reported)
                *reported = true;
            return false;
        }

        Mat34 mat_from_pose(const XrPosef& p)
        {
            const float x = p.orientation.x, y = p.orientation.y;
            const float z = p.orientation.z, w = p.orientation.w;
            Mat34 out{};
            out.m[0][0] = 1.0f - 2.0f * (y * y + z * z);
            out.m[0][1] = 2.0f * (x * y - z * w);
            out.m[0][2] = 2.0f * (x * z + y * w);
            out.m[1][0] = 2.0f * (x * y + z * w);
            out.m[1][1] = 1.0f - 2.0f * (x * x + z * z);
            out.m[1][2] = 2.0f * (y * z - x * w);
            out.m[2][0] = 2.0f * (x * z - y * w);
            out.m[2][1] = 2.0f * (y * z + x * w);
            out.m[2][2] = 1.0f - 2.0f * (x * x + y * y);
            out.m[0][3] = p.position.x;
            out.m[1][3] = p.position.y;
            out.m[2][3] = p.position.z;
            return out;
        }

        bool file_exists(const wchar_t* path)
        {
            const DWORD a = GetFileAttributesW(path);
            return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
        }

        bool has_extension(const std::vector<XrExtensionProperties>& list,
                           const char* name)
        {
            for (const XrExtensionProperties& e : list)
                if (strcmp(e.extensionName, name) == 0)
                    return true;
            return false;
        }

        XrResult create_instance()
        {
            uint32_t count = 0;
            XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0,
                                                                &count, nullptr);
            if (XR_FAILED(r))
                return r;
            std::vector<XrExtensionProperties> exts(count,
                {XR_TYPE_EXTENSION_PROPERTIES});
            r = xrEnumerateInstanceExtensionProperties(nullptr, count, &count,
                                                       exts.data());
            if (XR_FAILED(r))
                return r;

            std::string names;
            for (const XrExtensionProperties& e : exts)
            {
                names += e.extensionName;
                names += ' ';
            }
            log("xr: runtime extensions: %s", names.c_str());

            if (!has_extension(exts, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME))
            {
                set_error("the OpenXR runtime has no XR_KHR_vulkan_enable");
                return XR_ERROR_EXTENSION_NOT_PRESENT;
            }

            std::vector<const char*> enabled;
            enabled.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
            g_have_hp_controller = has_extension(exts,
                XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME);
            if (g_have_hp_controller)
                enabled.push_back(XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME);

            XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
            strcpy_s(info.applicationInfo.applicationName, "Tomb Raider Legend VR");
            info.applicationInfo.applicationVersion = 1;
            strcpy_s(info.applicationInfo.engineName, "trlvr");
            info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
            info.enabledExtensionCount = (uint32_t)enabled.size();
            info.enabledExtensionNames = enabled.data();
            return xrCreateInstance(&info, &g_instance);
        }

        template <typename Fn>
        bool load_proc(const char* name, Fn* out)
        {
            return XR_SUCCEEDED(xrGetInstanceProcAddr(g_instance, name,
                       (PFN_xrVoidFunction*)out)) && *out;
        }

        // The two-call idiom for a space-separated extension list.
        template <typename Fn>
        bool extension_string(Fn fn, std::string* out)
        {
            uint32_t size = 0;
            if (XR_FAILED(fn(g_instance, g_system, 0, &size, nullptr)))
                return false;
            std::vector<char> buffer(size + 1, 0);
            if (size && XR_FAILED(fn(g_instance, g_system, size, &size,
                                     buffer.data())))
                return false;
            *out = buffer.data();
            return true;
        }

        void destroy_instance()
        {
            if (g_instance != XR_NULL_HANDLE)
                xrDestroyInstance(g_instance);
            g_instance = XR_NULL_HANDLE;
            g_system = XR_NULL_SYSTEM_ID;
            g_action_set = XR_NULL_HANDLE;
        }

        // --- actions ------------------------------------------------------

        XrPath path(const char* text)
        {
            XrPath p = XR_NULL_PATH;
            xrStringToPath(g_instance, text, &p);
            return p;
        }

        XrAction make_action(const char* name, const char* label, XrActionType type)
        {
            XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
            strcpy_s(info.actionName, name);
            strcpy_s(info.localizedActionName, label);
            info.actionType = type;
            info.countSubactionPaths = 2;
            info.subactionPaths = g_hand_path;
            XrAction action = XR_NULL_HANDLE;
            if (!ok(xrCreateAction(g_action_set, &info, &action), name))
                return XR_NULL_HANDLE;
            return action;
        }

        struct Binding
        {
            XrAction* action;
            int hand;               // 0 left, 1 right, -1 both
            const char* component;  // below /user/hand/<side>
        };

        void suggest(const char* profile, const Binding* bindings, int count)
        {
            std::vector<XrActionSuggestedBinding> list;
            for (int i = 0; i < count; ++i)
            {
                const Binding& b = bindings[i];
                if (*b.action == XR_NULL_HANDLE)
                    continue;
                for (int hand = 0; hand < 2; ++hand)
                {
                    if (b.hand >= 0 && b.hand != hand)
                        continue;
                    char full[XR_MAX_PATH_LENGTH];
                    snprintf(full, sizeof(full), "/user/hand/%s%s",
                             hand == 0 ? "left" : "right", b.component);
                    list.push_back({*b.action, path(full)});
                }
            }
            XrInteractionProfileSuggestedBinding info{
                XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            info.interactionProfile = path(profile);
            info.countSuggestedBindings = (uint32_t)list.size();
            info.suggestedBindings = list.data();
            const XrResult r = xrSuggestInteractionProfileBindings(g_instance, &info);
            if (XR_FAILED(r))
                log("xr: bindings for %s were not accepted: %s", profile,
                    result_name(r));
        }

        bool create_actions()
        {
            if (g_action_set != XR_NULL_HANDLE)
                return true;
            XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};
            strcpy_s(set.actionSetName, "gameplay");
            strcpy_s(set.localizedActionSetName, "Tomb Raider: Legend");
            if (!ok(xrCreateActionSet(g_instance, &set, &g_action_set),
                    "xrCreateActionSet"))
                return false;

            g_hand_path[0] = path("/user/hand/left");
            g_hand_path[1] = path("/user/hand/right");

            g_act_stick = make_action("stick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT);
            g_act_trigger = make_action("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT);
            g_act_squeeze = make_action("squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT);
            g_act_button[XrButtonStick] = make_action("stick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT);
            g_act_button[XrButtonPrimary] = make_action("primary", "A / X button", XR_ACTION_TYPE_BOOLEAN_INPUT);
            g_act_button[XrButtonSecondary] = make_action("secondary", "B / Y button", XR_ACTION_TYPE_BOOLEAN_INPUT);
            g_act_button[XrButtonTrackpad] = make_action("trackpad", "Trackpad press", XR_ACTION_TYPE_BOOLEAN_INPUT);
            g_act_button[XrButtonMenu] = make_action("menu", "Menu button", XR_ACTION_TYPE_BOOLEAN_INPUT);
            g_act_button[XrTouchTrigger] = make_action("trigger_touch", "Finger on the trigger", XR_ACTION_TYPE_BOOLEAN_INPUT);
            g_act_button[XrTouchThumb] = make_action("thumb_touch", "Thumb on the controller", XR_ACTION_TYPE_BOOLEAN_INPUT);
            g_act_grip = make_action("grip_pose", "Hand grip pose", XR_ACTION_TYPE_POSE_INPUT);
            g_act_haptic = make_action("haptic", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT);

            // Only components the profile defines: one path it does not have
            // and the runtime rejects the whole suggestion.
            const Binding touch[] = {
                {&g_act_stick, -1, "/input/thumbstick"},
                {&g_act_button[XrButtonStick], -1, "/input/thumbstick/click"},
                {&g_act_trigger, -1, "/input/trigger/value"},
                {&g_act_squeeze, -1, "/input/squeeze/value"},
                {&g_act_button[XrButtonPrimary], 0, "/input/x/click"},
                {&g_act_button[XrButtonSecondary], 0, "/input/y/click"},
                {&g_act_button[XrButtonMenu], 0, "/input/menu/click"},
                {&g_act_button[XrButtonPrimary], 1, "/input/a/click"},
                {&g_act_button[XrButtonSecondary], 1, "/input/b/click"},
                {&g_act_button[XrTouchTrigger], -1, "/input/trigger/touch"},
                {&g_act_button[XrTouchThumb], -1, "/input/thumbstick/touch"},
                {&g_act_button[XrTouchThumb], -1, "/input/thumbrest/touch"},
                {&g_act_button[XrTouchThumb], 0, "/input/x/touch"},
                {&g_act_button[XrTouchThumb], 0, "/input/y/touch"},
                {&g_act_button[XrTouchThumb], 1, "/input/a/touch"},
                {&g_act_button[XrTouchThumb], 1, "/input/b/touch"},
                {&g_act_grip, -1, "/input/grip/pose"},
                {&g_act_haptic, -1, "/output/haptic"},
            };
            suggest("/interaction_profiles/oculus/touch_controller", touch,
                    (int)(sizeof(touch) / sizeof(touch[0])));

            const Binding index[] = {
                {&g_act_stick, -1, "/input/thumbstick"},
                {&g_act_button[XrButtonStick], -1, "/input/thumbstick/click"},
                {&g_act_trigger, -1, "/input/trigger/value"},
                {&g_act_squeeze, -1, "/input/squeeze/value"},
                {&g_act_button[XrButtonPrimary], -1, "/input/a/click"},
                {&g_act_button[XrButtonSecondary], -1, "/input/b/click"},
                {&g_act_button[XrButtonTrackpad], -1, "/input/trackpad/force"},
                {&g_act_button[XrTouchTrigger], -1, "/input/trigger/touch"},
                {&g_act_button[XrTouchThumb], -1, "/input/thumbstick/touch"},
                {&g_act_button[XrTouchThumb], -1, "/input/a/touch"},
                {&g_act_button[XrTouchThumb], -1, "/input/b/touch"},
                {&g_act_button[XrTouchThumb], -1, "/input/trackpad/touch"},
                {&g_act_grip, -1, "/input/grip/pose"},
                {&g_act_haptic, -1, "/output/haptic"},
            };
            suggest("/interaction_profiles/valve/index_controller", index,
                    (int)(sizeof(index) / sizeof(index[0])));

            const Binding wmr[] = {
                {&g_act_stick, -1, "/input/thumbstick"},
                {&g_act_button[XrButtonStick], -1, "/input/thumbstick/click"},
                {&g_act_trigger, -1, "/input/trigger/value"},
                {&g_act_squeeze, -1, "/input/squeeze/click"},
                {&g_act_button[XrButtonMenu], -1, "/input/menu/click"},
                {&g_act_button[XrButtonTrackpad], -1, "/input/trackpad/click"},
                {&g_act_grip, -1, "/input/grip/pose"},
                {&g_act_haptic, -1, "/output/haptic"},
            };
            suggest("/interaction_profiles/microsoft/motion_controller", wmr,
                    (int)(sizeof(wmr) / sizeof(wmr[0])));

            if (g_have_hp_controller)
            {
                const Binding hp[] = {
                    {&g_act_stick, -1, "/input/thumbstick"},
                    {&g_act_button[XrButtonStick], -1, "/input/thumbstick/click"},
                    {&g_act_trigger, -1, "/input/trigger/value"},
                    {&g_act_squeeze, -1, "/input/squeeze/value"},
                    {&g_act_button[XrButtonPrimary], 0, "/input/x/click"},
                    {&g_act_button[XrButtonSecondary], 0, "/input/y/click"},
                    {&g_act_button[XrButtonPrimary], 1, "/input/a/click"},
                    {&g_act_button[XrButtonSecondary], 1, "/input/b/click"},
                    {&g_act_button[XrButtonMenu], -1, "/input/menu/click"},
                    {&g_act_grip, -1, "/input/grip/pose"},
                    {&g_act_haptic, -1, "/output/haptic"},
                };
                suggest("/interaction_profiles/hp/mixed_reality_controller", hp,
                        (int)(sizeof(hp) / sizeof(hp[0])));
            }

            const Binding simple[] = {
                {&g_act_trigger, -1, "/input/select/click"},
                {&g_act_button[XrButtonMenu], -1, "/input/menu/click"},
                {&g_act_grip, -1, "/input/grip/pose"},
                {&g_act_haptic, -1, "/output/haptic"},
            };
            suggest("/interaction_profiles/khr/simple_controller", simple,
                    (int)(sizeof(simple) / sizeof(simple[0])));
            return true;
        }

        void attach_actions()
        {
            g_actions_attached = false;
            g_grip_space[0] = g_grip_space[1] = XR_NULL_HANDLE;
            if (!create_actions())
                return;
            XrSessionActionSetsAttachInfo attach{
                XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
            attach.countActionSets = 1;
            attach.actionSets = &g_action_set;
            if (!ok(xrAttachSessionActionSets(g_session, &attach),
                    "xrAttachSessionActionSets"))
                return;
            for (int hand = 0; hand < 2 && g_act_grip != XR_NULL_HANDLE; ++hand)
            {
                XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO};
                space.action = g_act_grip;
                space.subactionPath = g_hand_path[hand];
                space.poseInActionSpace.orientation.w = 1.0f;
                ok(xrCreateActionSpace(g_session, &space, &g_grip_space[hand]),
                   "xrCreateActionSpace (grip)");
            }
            g_actions_attached = true;
            g_profile_dirty = true;
        }

        void read_profiles()
        {
            g_profile_dirty = false;
            for (int hand = 0; hand < 2; ++hand)
            {
                XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
                char text[XR_MAX_PATH_LENGTH] = "";
                if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(g_session,
                        g_hand_path[hand], &state)) &&
                    state.interactionProfile != XR_NULL_PATH)
                {
                    uint32_t length = 0;
                    xrPathToString(g_instance, state.interactionProfile,
                                   sizeof(text), &length, text);
                }
                if (strcmp(text, g_profile[hand]) == 0)
                    continue;
                strcpy_s(g_profile[hand], text);
                g_controller[hand] =
                    !text[0] ? XrControllerNone
                    : strstr(text, "oculus/touch") ? XrControllerTouch
                    : strstr(text, "valve/index") ? XrControllerIndex
                    : strstr(text, "microsoft/motion") ? XrControllerWmr
                    : strstr(text, "hp/mixed_reality") ? XrControllerHpReverb
                    : XrControllerSimple;
                log("xr: %s hand interaction profile: %s",
                    hand == 0 ? "left" : "right", text[0] ? text : "(none)");
            }
        }

        void sync_actions()
        {
            g_input_active = false;
            if (!g_actions_attached || g_state != XR_SESSION_STATE_FOCUSED)
                return;
            XrActiveActionSet active{g_action_set, XR_NULL_PATH};
            XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
            sync.countActiveActionSets = 1;
            sync.activeActionSets = &active;
            static bool reported = false;
            const XrResult r = xrSyncActions(g_session, &sync);
            if (!ok(r, "xrSyncActions", &reported))
                return;
            g_input_active = r == XR_SUCCESS;
            if (g_profile_dirty)
                read_profiles();
        }

        // --- events -------------------------------------------------------

        const char* state_name(XrSessionState s)
        {
            switch (s)
            {
            case XR_SESSION_STATE_IDLE: return "IDLE";
            case XR_SESSION_STATE_READY: return "READY";
            case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
            case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
            case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
            case XR_SESSION_STATE_STOPPING: return "STOPPING";
            case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
            case XR_SESSION_STATE_EXITING: return "EXITING";
            default: return "UNKNOWN";
            }
        }

        void poll_events()
        {
            if (g_instance == XR_NULL_HANDLE)
                return;
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            while (xrPollEvent(g_instance, &event) == XR_SUCCESS)
            {
                switch (event.type)
                {
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                {
                    const XrEventDataSessionStateChanged* changed =
                        (const XrEventDataSessionStateChanged*)&event;
                    if (changed->session != g_session)
                        break;
                    g_state = changed->state;
                    log("xr: session state %s", state_name(g_state));
                    if (g_state == XR_SESSION_STATE_READY)
                    {
                        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                        begin.primaryViewConfigurationType =
                            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        g_running = ok(xrBeginSession(g_session, &begin),
                                       "xrBeginSession");
                    }
                    else if (g_state == XR_SESSION_STATE_STOPPING)
                    {
                        g_running = false;
                        g_frame_begun = false;
                        ok(xrEndSession(g_session), "xrEndSession");
                    }
                    else if (g_state == XR_SESSION_STATE_EXITING ||
                             g_state == XR_SESSION_STATE_LOSS_PENDING)
                    {
                        // The headset went away. The game carries on flat;
                        // nothing here restarts the session.
                        g_running = false;
                        g_frame_begun = false;
                    }
                    break;
                }
                case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                    g_profile_dirty = true;
                    break;
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                    log("xr: the runtime is shutting down (instance loss pending)");
                    g_running = false;
                    g_frame_begun = false;
                    break;
                default:
                    break;
                }
                event = {XR_TYPE_EVENT_DATA_BUFFER};
            }
        }

        void locate(XrSpace space, XrTime time, TrackedPose* out)
        {
            *out = TrackedPose{};
            if (space == XR_NULL_HANDLE)
                return;
            XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            location.next = &velocity;
            if (XR_FAILED(xrLocateSpace(space, g_local, time, &location)))
                return;
            const XrSpaceLocationFlags need =
                XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                XR_SPACE_LOCATION_POSITION_VALID_BIT;
            out->bDeviceIsConnected = true;
            out->bPoseIsValid = (location.locationFlags & need) == need;
            if (!out->bPoseIsValid)
                return;
            out->mDeviceToAbsoluteTracking = mat_from_pose(location.pose);
            if (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)
            {
                out->vVelocity.v[0] = velocity.linearVelocity.x;
                out->vVelocity.v[1] = velocity.linearVelocity.y;
                out->vVelocity.v[2] = velocity.linearVelocity.z;
            }
            if (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)
            {
                out->vAngularVelocity.v[0] = velocity.angularVelocity.x;
                out->vAngularVelocity.v[1] = velocity.angularVelocity.y;
                out->vAngularVelocity.v[2] = velocity.angularVelocity.z;
            }
        }
    }

    // --- instance -----------------------------------------------------------

    bool xr_instance_ready()
    {
        return g_instance != XR_NULL_HANDLE && g_system != XR_NULL_SYSTEM_ID;
    }

    const char* xr_last_error() { return g_error; }
    const char* xr_system_name() { return g_system_name; }

    bool xr_instance_init()
    {
        if (xr_instance_ready())
            return true;
        g_error[0] = 0;

        // The loader reads XR_RUNTIME_JSON at xrCreateInstance, so setting it
        // here is early enough.
        if (config().xr_runtime_json[0])
        {
            SetEnvironmentVariableW(L"XR_RUNTIME_JSON", config().xr_runtime_json);
            log("xr: runtime manifest from trlvr.ini: %S", config().xr_runtime_json);
        }

        XrResult r = create_instance();
        if (r == XR_ERROR_RUNTIME_UNAVAILABLE && !config().xr_runtime_json[0])
        {
            // No 32-bit runtime is registered (HKLM\SOFTWARE\WOW6432Node\
            // Khronos\OpenXR\1\ActiveRuntime). Virtual Desktop ships one;
            // use it if it is installed.
            wchar_t program_files[MAX_PATH]{};
            wchar_t vdxr[MAX_PATH]{};
            if (GetEnvironmentVariableW(L"ProgramW6432", program_files, MAX_PATH))
                swprintf_s(vdxr, L"%s\\Virtual Desktop Streamer\\OpenXR\\"
                           L"virtualdesktop-openxr-32.json", program_files);
            if (vdxr[0] && file_exists(vdxr))
            {
                log("xr: no active 32-bit OpenXR runtime; trying %S", vdxr);
                SetEnvironmentVariableW(L"XR_RUNTIME_JSON", vdxr);
                r = create_instance();
            }
        }
        if (XR_FAILED(r))
        {
            if (!g_error[0])
                set_error("xrCreateInstance failed: %s%s", result_name(r),
                          r == XR_ERROR_RUNTIME_UNAVAILABLE
                              ? " (no 32-bit OpenXR runtime is active)" : "");
            destroy_instance();
            return false;
        }

        XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(xrGetInstanceProperties(g_instance, &props)))
            log("xr: runtime %s %u.%u.%u", props.runtimeName,
                (unsigned)XR_VERSION_MAJOR(props.runtimeVersion),
                (unsigned)XR_VERSION_MINOR(props.runtimeVersion),
                (unsigned)XR_VERSION_PATCH(props.runtimeVersion));

        if (!load_proc("xrGetVulkanGraphicsRequirementsKHR", &p_GetVulkanGraphicsRequirements) ||
            !load_proc("xrGetVulkanInstanceExtensionsKHR", &p_GetVulkanInstanceExtensions) ||
            !load_proc("xrGetVulkanDeviceExtensionsKHR", &p_GetVulkanDeviceExtensions) ||
            !load_proc("xrGetVulkanGraphicsDeviceKHR", &p_GetVulkanGraphicsDevice))
        {
            set_error("the runtime is missing XR_KHR_vulkan_enable entry points");
            destroy_instance();
            return false;
        }

        XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO};
        get.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        r = xrGetSystem(g_instance, &get, &g_system);
        if (XR_FAILED(r))
        {
            set_error("xrGetSystem failed: %s%s", result_name(r),
                      r == XR_ERROR_FORM_FACTOR_UNAVAILABLE
                          ? " (headset not connected)" : "");
            destroy_instance();
            return false;
        }

        XrSystemProperties system{XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(xrGetSystemProperties(g_instance, g_system, &system)))
            strcpy_s(g_system_name, system.systemName);

        // DXVK creates the Vulkan instance and device, so it has to be told
        // which extensions the runtime wants on each. The fork's OpenXR
        // extension provider reads these.
        std::string instance_exts, device_exts;
        if (!extension_string(p_GetVulkanInstanceExtensions, &instance_exts) ||
            !extension_string(p_GetVulkanDeviceExtensions, &device_exts))
        {
            set_error("could not read the runtime's Vulkan extension lists");
            destroy_instance();
            return false;
        }
        SetEnvironmentVariableA("TRLVR_XR_VK_INSTANCE_EXTENSIONS", instance_exts.c_str());
        SetEnvironmentVariableA("TRLVR_XR_VK_DEVICE_EXTENSIONS", device_exts.c_str());
        log("xr: Vulkan instance extensions: %s", instance_exts.c_str());
        log("xr: Vulkan device extensions: %s", device_exts.c_str());
        return true;
    }

    void xr_instance_shutdown()
    {
        xr_session_destroy();
        destroy_instance();
    }

    bool xr_recommended_size(unsigned* width, unsigned* height)
    {
        if (!xr_instance_ready())
            return false;
        XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                            {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        uint32_t count = 0;
        if (XR_FAILED(xrEnumerateViewConfigurationViews(g_instance, g_system,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views)) ||
            count < 1)
            return false;
        if (width) *width = views[0].recommendedImageRectWidth;
        if (height) *height = views[0].recommendedImageRectHeight;
        return true;
    }

    // --- session ------------------------------------------------------------

    bool xr_session_exists() { return g_session != XR_NULL_HANDLE; }

    bool xr_session_create(const XrVulkanHandles& vk)
    {
        if (g_session != XR_NULL_HANDLE)
            return true;
        if (!xr_instance_ready())
            return false;

        // Mandatory before xrCreateSession, whatever it says.
        XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        if (!ok(p_GetVulkanGraphicsRequirements(g_instance, g_system, &req),
                "xrGetVulkanGraphicsRequirementsKHR"))
            return false;

        // The runtime names the GPU the headset is on. DXVK has already
        // chosen one; a mismatch (a laptop's other GPU) cannot work.
        VkPhysicalDevice wanted = VK_NULL_HANDLE;
        if (ok(p_GetVulkanGraphicsDevice(g_instance, g_system,
                   (VkInstance)vk.instance, &wanted),
               "xrGetVulkanGraphicsDeviceKHR") &&
            wanted != (VkPhysicalDevice)vk.physical_device)
            log("xr: WARNING -- the runtime wants a different GPU from the one "
                "DXVK is rendering on; the session will probably be refused");

        XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
        binding.instance = (VkInstance)vk.instance;
        binding.physicalDevice = (VkPhysicalDevice)vk.physical_device;
        binding.device = (VkDevice)vk.device;
        binding.queueFamilyIndex = vk.queue_family;
        binding.queueIndex = vk.queue_index;

        XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
        info.next = &binding;
        info.systemId = g_system;
        log_set_activity("xrCreateSession");
        const XrResult r = xrCreateSession(g_instance, &info, &g_session);
        log_set_activity(nullptr);
        if (!ok(r, "xrCreateSession"))
        {
            g_session = XR_NULL_HANDLE;
            return false;
        }
        g_state = XR_SESSION_STATE_UNKNOWN;
        g_running = false;
        g_frame_begun = false;

        XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space.poseInReferenceSpace.orientation.w = 1.0f;
        // LOCAL is the seated space: origin at the head when the runtime
        // was last recentred, gravity-aligned.
        space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        const bool have_local = ok(xrCreateReferenceSpace(g_session, &space, &g_local),
                                   "xrCreateReferenceSpace (LOCAL)");
        space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        const bool have_view = ok(xrCreateReferenceSpace(g_session, &space, &g_view),
                                  "xrCreateReferenceSpace (VIEW)");
        if (!have_local || !have_view)
        {
            xr_session_destroy();
            return false;
        }

        attach_actions();
        log("xr: session created (Vulkan queue family %u, index %u)",
            vk.queue_family, vk.queue_index);
        return true;
    }

    void xr_session_destroy()
    {
        xr_swapchain_destroy();
        if (g_session != XR_NULL_HANDLE)
        {
            // Spaces and action spaces are children of the session.
            xrDestroySession(g_session);
            log("xr: session destroyed");
        }
        g_session = XR_NULL_HANDLE;
        g_local = g_view = XR_NULL_HANDLE;
        g_grip_space[0] = g_grip_space[1] = XR_NULL_HANDLE;
        g_actions_attached = false;
        g_input_active = false;
        g_running = false;
        g_frame_begun = false;
        g_state = XR_SESSION_STATE_UNKNOWN;
        // An attached action set is immutable and tied to that session's
        // bindings; the next session gets a fresh one.
        if (g_action_set != XR_NULL_HANDLE)
        {
            xrDestroyActionSet(g_action_set);
            g_action_set = XR_NULL_HANDLE;
        }
    }

    // --- frame --------------------------------------------------------------

    bool xr_frame_begun() { return g_frame_begun; }
    double xr_frame_wait_ms() { return g_wait_ms; }

    bool xr_frame_should_render()
    {
        return g_frame_begun && g_frame.shouldRender == XR_TRUE;
    }

    bool xr_frame_wait(XrFrame* out)
    {
        if (g_session == XR_NULL_HANDLE)
            return false;
        poll_events();
        // A frame still open is the caller's to end first; its poses are not
        // refreshed here.
        if (!g_running || g_frame_begun)
            return false;

        LARGE_INTEGER frequency{}, before{}, after{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&before);
        static bool wait_reported = false, begin_reported = false;
        g_frame = {XR_TYPE_FRAME_STATE};
        log_set_activity("xrWaitFrame");
        XrResult r = xrWaitFrame(g_session, nullptr, &g_frame);
        log_set_activity(nullptr);
        if (!ok(r, "xrWaitFrame", &wait_reported))
            return false;
        QueryPerformanceCounter(&after);
        g_wait_ms = double(after.QuadPart - before.QuadPart) * 1000.0 /
                    double(frequency.QuadPart);

        log_set_activity("xrBeginFrame");
        r = xrBeginFrame(g_session, nullptr);
        log_set_activity(nullptr);
        if (!ok(r, "xrBeginFrame", &begin_reported))
            return false;
        g_frame_begun = true;

        sync_actions();
        if (!out)
            return true;

        const XrTime time = g_frame.predictedDisplayTime;
        locate(g_view, time, &out->poses[kPoseHmd]);
        for (int hand = 0; hand < 2; ++hand)
        {
            if (g_input_active)
                locate(g_grip_space[hand], time, &out->poses[kPoseLeft + hand]);
            else
                out->poses[kPoseLeft + hand] = TrackedPose{};
        }

        XrViewLocateInfo where{XR_TYPE_VIEW_LOCATE_INFO};
        where.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        where.displayTime = time;
        XrViewState state{XR_TYPE_VIEW_STATE};
        uint32_t count = 0;
        const XrViewStateFlags need = XR_VIEW_STATE_ORIENTATION_VALID_BIT |
                                      XR_VIEW_STATE_POSITION_VALID_BIT;

        // In LOCAL space, for the layer.
        where.space = g_local;
        g_layer_views[0] = g_layer_views[1] = {XR_TYPE_VIEW};
        g_layer_views_valid =
            XR_SUCCEEDED(xrLocateViews(g_session, &where, &state, 2, &count,
                                       g_layer_views)) &&
            count == 2 && (state.viewStateFlags & need) == need;

        // In VIEW space, which is each eye relative to the head.
        where.space = g_view;
        XrView eyes[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        state = {XR_TYPE_VIEW_STATE};
        out->eyes_valid =
            XR_SUCCEEDED(xrLocateViews(g_session, &where, &state, 2, &count,
                                       eyes)) &&
            count == 2 && (state.viewStateFlags & need) == need;
        if (out->eyes_valid)
        {
            for (int i = 0; i < 2; ++i)
            {
                XrEyeView& eye = out->eyes[i];
                eye.tan_left = tanf(eyes[i].fov.angleLeft);
                eye.tan_right = tanf(eyes[i].fov.angleRight);
                // EyeInfo's "top" is the down edge and "bottom" the up edge.
                eye.tan_top = tanf(eyes[i].fov.angleDown);
                eye.tan_bottom = tanf(eyes[i].fov.angleUp);
                eye.eye_to_head = mat_from_pose(eyes[i].pose);
            }
        }
        return true;
    }

    // --- swapchain ----------------------------------------------------------

    bool xr_swapchain_matches(unsigned width, unsigned height)
    {
        return g_swapchain != XR_NULL_HANDLE && g_swapchain_w == width &&
               g_swapchain_h == height;
    }

    int xr_swapchain_format() { return (int)g_swapchain_format; }

    void xr_swapchain_destroy()
    {
        if (g_swapchain != XR_NULL_HANDLE)
            xrDestroySwapchain(g_swapchain);
        g_swapchain = XR_NULL_HANDLE;
        g_swapchain_w = g_swapchain_h = 0;
        g_swapchain_images.clear();
        g_swapchain_acquired = false;
    }

    bool xr_swapchain_create(unsigned width, unsigned height)
    {
        xr_swapchain_destroy();
        if (g_session == XR_NULL_HANDLE || !width || !height)
            return false;

        uint32_t count = 0;
        if (!ok(xrEnumerateSwapchainFormats(g_session, 0, &count, nullptr),
                "xrEnumerateSwapchainFormats"))
            return false;
        std::vector<int64_t> formats(count);
        xrEnumerateSwapchainFormats(g_session, count, &count, formats.data());

        // The game's back buffer is BGRA8 holding gamma-encoded colour. An
        // sRGB swapchain of the same layout takes it bit for bit and tells
        // the compositor how to read it. UNORM would be read as linear and
        // come out too dark; RGBA needs a converting blit instead of a copy.
        const int64_t wanted[] = { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
                                   VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
        g_swapchain_format = 0;
        for (int64_t want : wanted)
        {
            for (int64_t have : formats)
                if (have == want)
                {
                    g_swapchain_format = want;
                    break;
                }
            if (g_swapchain_format)
                break;
        }
        if (!g_swapchain_format)
        {
            std::string list;
            for (int64_t f : formats)
                list += std::to_string((long long)f) + " ";
            log("xr: the runtime offers no 8-bit RGBA swapchain format "
                "(VkFormat list: %s)", list.c_str());
            return false;
        }

        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                          XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                          XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT |
                          XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = g_swapchain_format;
        info.sampleCount = 1;
        info.width = width;
        info.height = height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        log_set_activity("xrCreateSwapchain");
        const XrResult r = xrCreateSwapchain(g_session, &info, &g_swapchain);
        log_set_activity(nullptr);
        if (!ok(r, "xrCreateSwapchain"))
        {
            g_swapchain = XR_NULL_HANDLE;
            return false;
        }

        count = 0;
        xrEnumerateSwapchainImages(g_swapchain, 0, &count, nullptr);
        g_swapchain_images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        if (!count ||
            !ok(xrEnumerateSwapchainImages(g_swapchain, count, &count,
                    (XrSwapchainImageBaseHeader*)g_swapchain_images.data()),
                "xrEnumerateSwapchainImages"))
        {
            xr_swapchain_destroy();
            return false;
        }
        g_swapchain_w = width;
        g_swapchain_h = height;
        log("xr: swapchain %ux%u, VkFormat %d, %u images", width, height,
            (int)g_swapchain_format, count);
        return true;
    }

    bool xr_swapchain_acquire(uint64_t* vk_image)
    {
        if (g_swapchain == XR_NULL_HANDLE || g_swapchain_acquired || !vk_image)
            return false;
        static bool acquire_reported = false, wait_reported = false;
        uint32_t index = 0;
        log_set_activity("xrAcquireSwapchainImage");
        XrResult r = xrAcquireSwapchainImage(g_swapchain, nullptr, &index);
        log_set_activity(nullptr);
        if (!ok(r, "xrAcquireSwapchainImage", &acquire_reported))
            return false;
        g_swapchain_acquired = true;

        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        log_set_activity("xrWaitSwapchainImage");
        r = xrWaitSwapchainImage(g_swapchain, &wait);
        log_set_activity(nullptr);
        if (!ok(r, "xrWaitSwapchainImage", &wait_reported) ||
            index >= g_swapchain_images.size())
        {
            xr_swapchain_release();
            return false;
        }
        *vk_image = (uint64_t)g_swapchain_images[index].image;
        return true;
    }

    void xr_swapchain_release()
    {
        if (g_swapchain == XR_NULL_HANDLE || !g_swapchain_acquired)
            return;
        static bool reported = false;
        log_set_activity("xrReleaseSwapchainImage");
        ok(xrReleaseSwapchainImage(g_swapchain, nullptr),
           "xrReleaseSwapchainImage", &reported);
        log_set_activity(nullptr);
        g_swapchain_acquired = false;
    }

    bool xr_frame_end(const XrRect* left, const XrRect* right)
    {
        if (g_session == XR_NULL_HANDLE || !g_frame_begun)
            return false;
        g_frame_begun = false;

        XrCompositionLayerProjectionView views[2] = {
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* layers[1] = {
            (const XrCompositionLayerBaseHeader*)&layer};

        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = g_frame.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

        const bool with_layer = left && right && g_layer_views_valid &&
            g_swapchain != XR_NULL_HANDLE && g_frame.shouldRender == XR_TRUE;
        if (with_layer)
        {
            const XrRect* rects[2] = {left, right};
            for (int i = 0; i < 2; ++i)
            {
                views[i].pose = g_layer_views[i].pose;
                views[i].fov = g_layer_views[i].fov;
                views[i].subImage.swapchain = g_swapchain;
                views[i].subImage.imageRect.offset = {rects[i]->x, rects[i]->y};
                views[i].subImage.imageRect.extent = {rects[i]->width,
                                                      rects[i]->height};
                views[i].subImage.imageArrayIndex = 0;
            }
            layer.space = g_local;
            layer.viewCount = 2;
            layer.views = views;
            end.layerCount = 1;
            end.layers = layers;
        }

        static bool reported = false;
        log_set_activity("xrEndFrame");
        const XrResult r = xrEndFrame(g_session, &end);
        log_set_activity(nullptr);
        return ok(r, "xrEndFrame", &reported) && with_layer;
    }

    // --- controllers --------------------------------------------------------

    bool xr_input_active() { return g_input_active; }

    bool xr_input_button(int hand, XrButton button)
    {
        if (!g_input_active || hand < 0 || hand > 1 || button < 0 ||
            button >= XrButtonCount || g_act_button[button] == XR_NULL_HANDLE)
            return false;
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = g_act_button[button];
        get.subactionPath = g_hand_path[hand];
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &get, &state)) &&
               state.isActive && state.currentState;
    }

    namespace
    {
        float float_state(XrAction action, int hand)
        {
            if (!g_input_active || hand < 0 || hand > 1 ||
                action == XR_NULL_HANDLE)
                return 0.0f;
            XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
            get.action = action;
            get.subactionPath = g_hand_path[hand];
            XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
            if (XR_FAILED(xrGetActionStateFloat(g_session, &get, &state)) ||
                !state.isActive)
                return 0.0f;
            return state.currentState;
        }
    }

    float xr_input_trigger(int hand) { return float_state(g_act_trigger, hand); }
    float xr_input_squeeze(int hand) { return float_state(g_act_squeeze, hand); }

    void xr_input_stick(int hand, float* x, float* y)
    {
        *x = *y = 0.0f;
        if (!g_input_active || hand < 0 || hand > 1 ||
            g_act_stick == XR_NULL_HANDLE)
            return;
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = g_act_stick;
        get.subactionPath = g_hand_path[hand];
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_FAILED(xrGetActionStateVector2f(g_session, &get, &state)) ||
            !state.isActive)
            return;
        *x = state.currentState.x;
        *y = state.currentState.y;
    }

    bool xr_input_haptic(int hand, float seconds, float frequency, float amplitude)
    {
        if (!g_input_active || hand < 0 || hand > 1 ||
            g_act_haptic == XR_NULL_HANDLE)
            return false;
        XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
        vibration.duration = (XrDuration)(seconds * 1.0e9f);
        vibration.frequency = frequency;
        vibration.amplitude = amplitude < 0.0f ? 0.0f
                            : amplitude > 1.0f ? 1.0f : amplitude;
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action = g_act_haptic;
        info.subactionPath = g_hand_path[hand];
        return XR_SUCCEEDED(xrApplyHapticFeedback(g_session, &info,
                   (const XrHapticBaseHeader*)&vibration));
    }

    XrControllerKind xr_input_controller(int hand)
    {
        return hand < 0 || hand > 1 ? XrControllerNone : g_controller[hand];
    }

    const char* xr_input_profile(int hand)
    {
        return hand < 0 || hand > 1 ? "" : g_profile[hand];
    }
}
