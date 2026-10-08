// Creates a Vulkan device and a swapchain on a headless surface, the way the emulator does when
// it runs without a display, and reports where things go wrong (including a crash inside the
// driver). For checking a Vulkan driver on a device without running the emulator.
//   vk_headless_probe [width height]
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

static void print_map_line(const char* label, unsigned long address) {
    FILE* maps = fopen("/proc/self/maps", "r");
    char line[512];
    while (maps != NULL && fgets(line, sizeof(line), maps) != NULL) {
        unsigned long begin = 0, end = 0;
        if (sscanf(line, "%lx-%lx", &begin, &end) == 2 && address >= begin && address < end) {
            printf("%s %#lx is +%#lx in: %s", label, address, address - begin, line);
            fclose(maps);
            return;
        }
    }
    if (maps != NULL) {
        fclose(maps);
    }
    printf("%s %#lx is not mapped\n", label, address);
}

static void on_crash(int signal_number, siginfo_t* info, void* raw_context) {
    const ucontext_t* context = raw_context;
    printf("CRASH signal=%d code=%d\n", signal_number, info->si_code);
    print_map_line("fault address", (unsigned long)info->si_addr);
    print_map_line("pc", (unsigned long)context->uc_mcontext.pc);
    print_map_line("lr", (unsigned long)context->uc_mcontext.regs[30]);
    fflush(stdout);
    _exit(1);
}

#define CHECK(call)                                                                               \
    do {                                                                                          \
        const VkResult check_result = (call);                                                     \
        printf("%s -> %d\n", #call, check_result);                                                \
        fflush(stdout);                                                                           \
        if (check_result != VK_SUCCESS) {                                                         \
            return 1;                                                                             \
        }                                                                                         \
    } while (0)

int main(int argc, char** argv) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_crash;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGSEGV, &action, NULL);

    const uint32_t width = argc > 2 ? (uint32_t)atoi(argv[1]) : 1280;
    const uint32_t height = argc > 2 ? (uint32_t)atoi(argv[2]) : 720;

    const char* instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                         VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = instance_extensions,
    };
    VkInstance instance;
    CHECK(vkCreateInstance(&instance_info, NULL, &instance));

    uint32_t count = 1;
    VkPhysicalDevice physical;
    vkEnumeratePhysicalDevices(instance, &count, &physical);
    if (count == 0) {
        printf("no physical device\n");
        return 1;
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    printf("device: %s\n", properties.deviceName);

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char* device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = device_extensions,
    };
    VkDevice device;
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));

    const VkHeadlessSurfaceCreateInfoEXT surface_info = {
        .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VkSurfaceKHR surface;
    CHECK(vkCreateHeadlessSurfaceEXT(instance, &surface_info, NULL, &surface));

    VkSurfaceCapabilitiesKHR capabilities;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities));
    VkSurfaceFormatKHR formats[32];
    uint32_t format_count = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats);
    VkPresentModeKHR modes[8];
    uint32_t mode_count = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count, modes);
    printf("surface: %u formats (first %d), %u modes (first %d), images %u..%u\n", format_count,
           formats[0].format, mode_count, modes[0], capabilities.minImageCount,
           capabilities.maxImageCount);

    const VkSwapchainCreateInfoKHR swapchain_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = capabilities.minImageCount > 3 ? capabilities.minImageCount : 3,
        .imageFormat = formats[0].format,
        .imageColorSpace = formats[0].colorSpace,
        .imageExtent = {width, height},
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = modes[0],
        .clipped = VK_TRUE,
    };
    VkSwapchainKHR swapchain;
    CHECK(vkCreateSwapchainKHR(device, &swapchain_info, NULL, &swapchain));

    uint32_t image_count = 0;
    CHECK(vkGetSwapchainImagesKHR(device, swapchain, &image_count, NULL));
    printf("swapchain: %u images\n", image_count);

    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);
    for (int frame = 0; frame < 3; ++frame) {
        uint32_t index = 0;
        CHECK(vkAcquireNextImageKHR(device, swapchain, 1000000000ull, VK_NULL_HANDLE,
                                    VK_NULL_HANDLE, &index));
        const VkPresentInfoKHR present = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &index,
        };
        CHECK(vkQueuePresentKHR(queue, &present));
    }
    printf("OK\n");
    return 0;
}
