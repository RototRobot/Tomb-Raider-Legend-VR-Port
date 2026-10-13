// OpenXR probe: what does the active 32-bit runtime offer?
//
// Standalone, no game and no D3D9. Creates an instance, lists extensions,
// asks for the system and the recommended eye size, then brings up a bare
// Vulkan device the way XR_KHR_vulkan_enable wants it and creates a session
// to read the swapchain formats, the real FOV and the interaction profile.
// Every step logs and carries on where it can, so a partial result still
// says how far the runtime got.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    XrInstance g_instance = XR_NULL_HANDLE;

    const char* result_name(XrResult r)
    {
        static char buffer[XR_MAX_RESULT_STRING_SIZE];
        if (g_instance && XR_SUCCEEDED(xrResultToString(g_instance, r, buffer)))
            return buffer;
        snprintf(buffer, sizeof(buffer), "%d", (int)r);
        return buffer;
    }

    bool check(XrResult r, const char* what)
    {
        if (XR_SUCCEEDED(r))
            return true;
        printf("FAILED %s: %s\n", what, result_name(r));
        return false;
    }

    std::vector<const char*> split(std::string& list)
    {
        std::vector<const char*> out;
        char* p = list.data();
        while (p && *p)
        {
            while (*p == ' ') *p++ = 0;
            if (!*p) break;
            out.push_back(p);
            p = strchr(p, ' ');
        }
        return out;
    }

    template <typename Fn>
    Fn vk_proc(HMODULE vk, const char* name)
    {
        return (Fn)GetProcAddress(vk, name);
    }
}

int main()
{
    uint32_t count = 0;
    if (!check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr),
               "xrEnumerateInstanceExtensionProperties"))
        return 1;
    std::vector<XrExtensionProperties> exts(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data());

    bool have_vk1 = false, have_vk2 = false, have_hp = false;
    printf("extensions (%u):\n", count);
    for (const auto& e : exts)
    {
        printf("  %s v%u\n", e.extensionName, e.extensionVersion);
        if (!strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)) have_vk1 = true;
        if (!strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) have_vk2 = true;
        if (!strcmp(e.extensionName, XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME)) have_hp = true;
    }
    printf("vulkan_enable=%d vulkan_enable2=%d hp_controller=%d\n", have_vk1, have_vk2, have_hp);
    if (!have_vk1)
    {
        printf("no XR_KHR_vulkan_enable; stopping\n");
        return 1;
    }

    const char* enabled[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ici.applicationInfo.applicationName, "trlvr_xrprobe");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled;
    if (!check(xrCreateInstance(&ici, &g_instance), "xrCreateInstance"))
        return 1;

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (check(xrGetInstanceProperties(g_instance, &ip), "xrGetInstanceProperties"))
        printf("runtime: %s %u.%u.%u\n", ip.runtimeName,
               XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
               XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    if (!check(xrGetSystem(g_instance, &sgi, &system), "xrGetSystem"))
        return 1;

    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (check(xrGetSystemProperties(g_instance, system, &sp), "xrGetSystemProperties"))
        printf("system: %s max swapchain %ux%u layers %u\n", sp.systemName,
               sp.graphicsProperties.maxSwapchainImageWidth,
               sp.graphicsProperties.maxSwapchainImageHeight,
               sp.graphicsProperties.maxLayerCount);

    XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                      {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (check(xrEnumerateViewConfigurationViews(g_instance, system,
                  XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, vcv),
              "xrEnumerateViewConfigurationViews"))
        for (uint32_t i = 0; i < count; ++i)
            printf("view %u: recommended %ux%u max %ux%u samples %u\n", i,
                   vcv[i].recommendedImageRectWidth, vcv[i].recommendedImageRectHeight,
                   vcv[i].maxImageRectWidth, vcv[i].maxImageRectHeight,
                   vcv[i].recommendedSwapchainSampleCount);

    // Extension functions are not exported by the loader.
    PFN_xrGetVulkanGraphicsRequirementsKHR xrGetVulkanGraphicsRequirementsKHR = nullptr;
    PFN_xrGetVulkanInstanceExtensionsKHR xrGetVulkanInstanceExtensionsKHR = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR xrGetVulkanDeviceExtensionsKHR = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR xrGetVulkanGraphicsDeviceKHR = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&xrGetVulkanGraphicsRequirementsKHR);
    xrGetInstanceProcAddr(g_instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&xrGetVulkanInstanceExtensionsKHR);
    xrGetInstanceProcAddr(g_instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&xrGetVulkanDeviceExtensionsKHR);
    xrGetInstanceProcAddr(g_instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction*)&xrGetVulkanGraphicsDeviceKHR);
    if (!xrGetVulkanGraphicsRequirementsKHR || !xrGetVulkanInstanceExtensionsKHR ||
        !xrGetVulkanDeviceExtensionsKHR || !xrGetVulkanGraphicsDeviceKHR)
    {
        printf("XR_KHR_vulkan_enable entry points missing\n");
        return 1;
    }

    XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (check(xrGetVulkanGraphicsRequirementsKHR(g_instance, system, &req),
              "xrGetVulkanGraphicsRequirementsKHR"))
        printf("vulkan api: min %u.%u max %u.%u\n",
               XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported),
               XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported));

    std::string inst_exts, dev_exts;
    if (check(xrGetVulkanInstanceExtensionsKHR(g_instance, system, 0, &count, nullptr),
              "xrGetVulkanInstanceExtensionsKHR"))
    {
        inst_exts.resize(count);
        xrGetVulkanInstanceExtensionsKHR(g_instance, system, count, &count, inst_exts.data());
        printf("vulkan instance extensions: %s\n", inst_exts.c_str());
    }
    if (check(xrGetVulkanDeviceExtensionsKHR(g_instance, system, 0, &count, nullptr),
              "xrGetVulkanDeviceExtensionsKHR"))
    {
        dev_exts.resize(count);
        xrGetVulkanDeviceExtensionsKHR(g_instance, system, count, &count, dev_exts.data());
        printf("vulkan device extensions: %s\n", dev_exts.c_str());
    }

    // A bare Vulkan device, enough to create a session with.
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk)
    {
        printf("vulkan-1.dll not found; stopping before the session\n");
        return 0;
    }
    auto vkCreateInstance = vk_proc<PFN_vkCreateInstance>(vk, "vkCreateInstance");
    auto vkGetPhysicalDeviceProperties = vk_proc<PFN_vkGetPhysicalDeviceProperties>(vk, "vkGetPhysicalDeviceProperties");
    auto vkGetPhysicalDeviceQueueFamilyProperties = vk_proc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(vk, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto vkCreateDevice = vk_proc<PFN_vkCreateDevice>(vk, "vkCreateDevice");

    auto inst_list = split(inst_exts);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo vici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    vici.pApplicationInfo = &app;
    vici.enabledExtensionCount = (uint32_t)inst_list.size();
    vici.ppEnabledExtensionNames = inst_list.data();
    VkInstance vk_instance = VK_NULL_HANDLE;
    VkResult vr = vkCreateInstance(&vici, nullptr, &vk_instance);
    if (vr != VK_SUCCESS)
    {
        printf("FAILED vkCreateInstance: %d\n", (int)vr);
        return 1;
    }

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    if (!check(xrGetVulkanGraphicsDeviceKHR(g_instance, system, vk_instance, &phys),
               "xrGetVulkanGraphicsDeviceKHR"))
        return 1;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("vulkan device: %s\n", props.deviceName);

    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qcount, qprops.data());
    uint32_t family = 0;
    for (uint32_t i = 0; i < qcount; ++i)
        if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }

    auto dev_list = split(dev_exts);
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)dev_list.size();
    dci.ppEnabledExtensionNames = dev_list.data();
    VkDevice device = VK_NULL_HANDLE;
    vr = vkCreateDevice(phys, &dci, nullptr, &device);
    if (vr != VK_SUCCESS)
    {
        printf("FAILED vkCreateDevice: %d\n", (int)vr);
        return 1;
    }

    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = vk_instance;
    binding.physicalDevice = phys;
    binding.device = device;
    binding.queueFamilyIndex = family;
    binding.queueIndex = 0;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    if (!check(xrCreateSession(g_instance, &sci, &session), "xrCreateSession"))
        return 1;
    printf("session created with XrGraphicsBindingVulkanKHR\n");

    if (check(xrEnumerateSwapchainFormats(session, 0, &count, nullptr), "xrEnumerateSwapchainFormats"))
    {
        std::vector<int64_t> formats(count);
        xrEnumerateSwapchainFormats(session, count, &count, formats.data());
        printf("swapchain formats (VkFormat):");
        for (int64_t f : formats) printf(" %lld", (long long)f);
        printf("\n");
    }

    if (check(xrEnumerateReferenceSpaces(session, 0, &count, nullptr), "xrEnumerateReferenceSpaces"))
    {
        std::vector<XrReferenceSpaceType> spaces(count);
        xrEnumerateReferenceSpaces(session, count, &count, spaces.data());
        printf("reference spaces:");
        for (auto s : spaces) printf(" %d", (int)s);
        printf("\n");
    }

    // A double-wide swapchain, as the side-by-side submit wants.
    {
        XrSwapchainCreateInfo sw{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sw.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                        XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                        XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sw.format = VK_FORMAT_B8G8R8A8_SRGB;
        sw.sampleCount = 1;
        sw.width = vcv[0].recommendedImageRectWidth * 2;
        sw.height = vcv[0].recommendedImageRectHeight;
        sw.faceCount = 1;
        sw.arraySize = 1;
        sw.mipCount = 1;
        XrSwapchain chain = XR_NULL_HANDLE;
        if (check(xrCreateSwapchain(session, &sw, &chain), "xrCreateSwapchain (double wide, BGRA8 sRGB)"))
        {
            xrEnumerateSwapchainImages(chain, 0, &count, nullptr);
            printf("double-wide swapchain %ux%u ok, %u images\n", sw.width, sw.height, count);
            xrDestroySwapchain(chain);
        }
    }

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XrSpace view_space = XR_NULL_HANDLE;
    check(xrCreateReferenceSpace(session, &rsci, &view_space), "xrCreateReferenceSpace(VIEW)");

    // Wait for the session to become ready, then run a few empty frames so
    // xrLocateViews has a real display time to answer for.
    bool running = false;
    int frames = 0;
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < 15000 && frames < 20)
    {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(g_instance, &ev) == XR_SUCCESS)
        {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                const auto* sc = (const XrEventDataSessionStateChanged*)&ev;
                printf("session state -> %d\n", (int)sc->state);
                if (sc->state == XR_SESSION_STATE_READY)
                {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    running = check(xrBeginSession(session, &bi), "xrBeginSession");
                }
                else if (sc->state == XR_SESSION_STATE_STOPPING)
                {
                    xrEndSession(session);
                    running = false;
                }
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!running)
        {
            Sleep(50);
            continue;
        }

        XrFrameState fs{XR_TYPE_FRAME_STATE};
        if (!check(xrWaitFrame(session, nullptr, &fs), "xrWaitFrame")) break;
        if (!check(xrBeginFrame(session, nullptr), "xrBeginFrame")) break;

        if (frames == 10 && view_space)
        {
            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = view_space;
            XrViewState vs{XR_TYPE_VIEW_STATE};
            XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            if (check(xrLocateViews(session, &vli, &vs, 2, &count, views), "xrLocateViews"))
            {
                printf("view state flags 0x%llx, period %lld ns\n",
                       (unsigned long long)vs.viewStateFlags, (long long)fs.predictedDisplayPeriod);
                for (int i = 0; i < 2; ++i)
                    printf("eye %d: tan l %.4f r %.4f u %.4f d %.4f  pos %.4f %.4f %.4f  quat %.4f %.4f %.4f %.4f\n", i,
                           tanf(views[i].fov.angleLeft), tanf(views[i].fov.angleRight),
                           tanf(views[i].fov.angleUp), tanf(views[i].fov.angleDown),
                           views[i].pose.position.x, views[i].pose.position.y, views[i].pose.position.z,
                           views[i].pose.orientation.x, views[i].pose.orientation.y,
                           views[i].pose.orientation.z, views[i].pose.orientation.w);
            }
        }

        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        if (!check(xrEndFrame(session, &fei), "xrEndFrame")) break;
        ++frames;
    }
    printf("frames run: %d%s\n", frames, running ? "" : " (session never became ready -- headset connected?)");

    xrDestroySession(session);
    xrDestroyInstance(g_instance);
    return 0;
}
