// Asks the active OpenXR runtime (or the one XR_RUNTIME_JSON names) what it offers an
// application that draws with Vulkan, as far as that goes without one: extensions, the system,
// the size it wants pictures at, and what it needs of the Vulkan instance and device.
//   xr_probe_win.exe

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace {

const char* ResultName(XrInstance instance, XrResult result) {
    static char buffer[XR_MAX_RESULT_STRING_SIZE];
    if (instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance, result, buffer))) {
        std::snprintf(buffer, sizeof(buffer), "%d", static_cast<int>(result));
    }
    return buffer;
}

template <typename Function>
Function GetFunction(XrInstance instance, const char* name) {
    PFN_xrVoidFunction function = nullptr;
    xrGetInstanceProcAddr(instance, name, &function);
    return reinterpret_cast<Function>(function);
}

} // namespace

int main() {
    if (const char* json = std::getenv("XR_RUNTIME_JSON")) {
        std::printf("runtime named by XR_RUNTIME_JSON: %s\n", json);
    } else {
        char path[1024]{};
        DWORD size = sizeof(path);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime",
                         RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS) {
            std::printf("the system's active runtime: %s\n", path);
        } else {
            std::printf("the system has no active OpenXR runtime\n");
        }
    }

    uint32_t count = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    if (XR_FAILED(result)) {
        std::printf("xrEnumerateInstanceExtensionProperties: %d\n", static_cast<int>(result));
        return 1;
    }
    std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data());
    std::printf("%u extensions:\n", count);
    for (const auto& extension : available) {
        std::printf("  %s (%u)\n", extension.extensionName, extension.extensionVersion);
    }
    const auto has = [&](const char* name) {
        return std::any_of(available.begin(), available.end(), [&](const auto& property) {
            return std::strcmp(property.extensionName, name) == 0;
        });
    };

    std::vector<const char*> extensions;
    for (const char* name :
         {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
          XR_EXT_HAND_TRACKING_EXTENSION_NAME, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,
          XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME}) {
        if (has(name)) {
            extensions.push_back(name);
        }
    }

    XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strcpy(instance_info.applicationInfo.applicationName, "Astro XR probe");
    instance_info.applicationInfo.applicationVersion = 1;
    instance_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    instance_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instance_info.enabledExtensionNames = extensions.data();
    XrInstance instance = XR_NULL_HANDLE;
    result = xrCreateInstance(&instance_info, &instance);
    std::printf("xrCreateInstance: %s\n", ResultName(XR_NULL_HANDLE, result));
    if (XR_FAILED(result)) {
        return 1;
    }
    XrInstanceProperties instance_properties{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance, &instance_properties);
    std::printf("runtime: %s %u.%u.%u\n", instance_properties.runtimeName,
                XR_VERSION_MAJOR(instance_properties.runtimeVersion),
                XR_VERSION_MINOR(instance_properties.runtimeVersion),
                XR_VERSION_PATCH(instance_properties.runtimeVersion));

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    result = xrGetSystem(instance, &system_info, &system);
    std::printf("xrGetSystem: %s\n", ResultName(instance, result));
    if (XR_FAILED(result)) {
        xrDestroyInstance(instance);
        return 2;
    }

    XrSystemHandTrackingPropertiesEXT hand_properties{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    if (has(XR_EXT_HAND_TRACKING_EXTENSION_NAME)) {
        system_properties.next = &hand_properties;
    }
    xrGetSystemProperties(instance, system, &system_properties);
    std::printf("system: %s, vendor %u, max swapchain %ux%u, %u layers, hand tracking %d\n",
                system_properties.systemName, system_properties.vendorId,
                system_properties.graphicsProperties.maxSwapchainImageWidth,
                system_properties.graphicsProperties.maxSwapchainImageHeight,
                system_properties.graphicsProperties.maxLayerCount,
                static_cast<int>(hand_properties.supportsHandTracking));

    uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      0, &view_count, nullptr);
    std::vector<XrViewConfigurationView> views(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      view_count, &view_count, views.data());
    for (uint32_t i = 0; i < view_count; ++i) {
        std::printf("view %u: recommended %ux%u, max %ux%u, samples %u\n", i,
                    views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight,
                    views[i].maxImageRectWidth, views[i].maxImageRectHeight,
                    views[i].recommendedSwapchainSampleCount);
    }

    if (has(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)) {
        const auto get_requirements = GetFunction<PFN_xrGetVulkanGraphicsRequirementsKHR>(
            instance, "xrGetVulkanGraphicsRequirementsKHR");
        XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        result = get_requirements(instance, system, &requirements);
        std::printf("Vulkan wanted: %u.%u to %u.%u (%s)\n",
                    XR_VERSION_MAJOR(requirements.minApiVersionSupported),
                    XR_VERSION_MINOR(requirements.minApiVersionSupported),
                    XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
                    XR_VERSION_MINOR(requirements.maxApiVersionSupported),
                    ResultName(instance, result));
        const auto instance_extensions = GetFunction<PFN_xrGetVulkanInstanceExtensionsKHR>(
            instance, "xrGetVulkanInstanceExtensionsKHR");
        const auto device_extensions = GetFunction<PFN_xrGetVulkanDeviceExtensionsKHR>(
            instance, "xrGetVulkanDeviceExtensionsKHR");
        uint32_t size = 0;
        instance_extensions(instance, system, 0, &size, nullptr);
        std::string names(size, '\0');
        result = instance_extensions(instance, system, size, &size, names.data());
        std::printf("Vulkan instance extensions wanted: %s (%s)\n", names.c_str(),
                    ResultName(instance, result));
        size = 0;
        device_extensions(instance, system, 0, &size, nullptr);
        names.assign(size, '\0');
        result = device_extensions(instance, system, size, &size, names.data());
        std::printf("Vulkan device extensions wanted: %s (%s)\n", names.c_str(),
                    ResultName(instance, result));
    }

    xrDestroyInstance(instance);
    return 0;
}
