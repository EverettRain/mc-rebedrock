#pragma once

// Vulkan 设备层的唯一所有者
// instance、surface、物理与逻辑设备、VMA 分配器、队列、命令池，连同它们的创建与销毁都在这里
// 渲染器只持有同名的非拥有副本，在 initialize 之后立即赋值
// 它内部对 device、allocator 的引用写法因此不变，但销毁责任始终在本类

#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>

#include "render/vulkan/VulkanResources.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

namespace mc::render {

inline constexpr const char* kPortabilityEnumeration = "VK_KHR_portability_enumeration";
inline constexpr const char* kPortabilitySubset = "VK_KHR_portability_subset";
inline constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
#ifndef NDEBUG
inline constexpr bool kRequestValidation = true;
#else
inline constexpr bool kRequestValidation = false;
#endif

struct QueueFamilyIndices final {
    std::optional<std::uint32_t> graphics;
    std::optional<std::uint32_t> present;

    [[nodiscard]] bool complete() const { return graphics.has_value() && present.has_value(); }
};

struct SwapchainSupport final {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

inline VKAPI_ATTR VkBool32 VKAPI_CALL
debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
              const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void*) {
    const char* prefix = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
                             ? "Vulkan validation error"
                             : "Vulkan validation";
    const char* messageId = callbackData->pMessageIdName != nullptr
                                ? callbackData->pMessageIdName
                                : "unknown VUID";
    std::cerr << prefix << " [" << messageId << "]: " << callbackData->pMessage << '\n';
    // 校验层回调不能跨 C ABI 抛异常，所以打印完整的 VUID/句柄信息后直接终止进程
    // 脚本化的烟测始终开着这道闸；交互运行或定点复现可以用这个开关手动打开同样的快速失败
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT &&
        (std::getenv("MC_REBEDROCK_SMOKE_TEST") != nullptr ||
         std::getenv("MC_REBEDROCK_FATAL_VALIDATION") != nullptr)) {
        std::cerr << "Fatal Vulkan validation error; aborting validation-gated run.\n"
                  << std::flush;
        std::abort();
    }
    return VK_FALSE;
}

[[nodiscard]] inline VkDebugUtilsMessengerCreateInfoEXT debugMessengerInfo() {
    auto info = vkStructure<VkDebugUtilsMessengerCreateInfoEXT>(
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;
    return info;
}

class VulkanDevice final {
  public:
    VulkanDevice() = default;
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;
    ~VulkanDevice() { destroy(); }

    // 按依赖顺序依次创建：instance → surface → 物理设备 → 逻辑设备 → 分配器 → 命令池
    void initialize(GLFWwindow* window) {
        createInstance();
        checkVk(glfwCreateWindowSurface(instance, window, nullptr, &surface),
                "glfwCreateWindowSurface");
        pickPhysicalDevice();
        createLogicalDevice();
        createAllocator();
        createCommandPool();
    }

    // 按依赖逆序销毁，可重复调用
    // 渲染器关闭时（在所有依赖设备的资源都已释放之后）显式调一次，析构函数再调一次
    void destroy() noexcept {
        if (device != VK_NULL_HANDLE) {
            if (commandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device, commandPool, nullptr);
                commandPool = VK_NULL_HANDLE;
            }
            if (allocator != VK_NULL_HANDLE) {
                vmaDestroyAllocator(allocator);
                allocator = VK_NULL_HANDLE;
            }
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        destroyDebugMessenger();
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    }

    [[nodiscard]] bool instanceExtensionAvailable(const char* wanted) const {
        std::uint32_t count = 0;
        checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
                "vkEnumerateInstanceExtensionProperties");
        std::vector<VkExtensionProperties> extensions(count);
        checkVk(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()),
                "vkEnumerateInstanceExtensionProperties");
        return std::ranges::any_of(extensions, [wanted](const auto& extension) {
            return std::strcmp(extension.extensionName, wanted) == 0;
        });
    }

    [[nodiscard]] bool validationLayerAvailable() const {
        std::uint32_t count = 0;
        checkVk(vkEnumerateInstanceLayerProperties(&count, nullptr),
                "vkEnumerateInstanceLayerProperties");
        std::vector<VkLayerProperties> layers(count);
        checkVk(vkEnumerateInstanceLayerProperties(&count, layers.data()),
                "vkEnumerateInstanceLayerProperties");
        return std::ranges::any_of(layers, [](const auto& layer) {
            return std::strcmp(layer.layerName, kValidationLayer) == 0;
        });
    }

    void createDebugMessenger() {
        if (!validationEnabled) {
            return;
        }
        const auto function = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (function == nullptr) {
            throw std::runtime_error("VK_EXT_debug_utils is enabled but unavailable");
        }
        const auto info = debugMessengerInfo();
        checkVk(function(instance, &info, nullptr, &debugMessenger),
                "vkCreateDebugUtilsMessengerEXT");
    }

    void destroyDebugMessenger() noexcept {
        if (debugMessenger == VK_NULL_HANDLE || instance == VK_NULL_HANDLE) {
            return;
        }
        const auto function = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (function != nullptr) {
            function(instance, debugMessenger, nullptr);
        }
        debugMessenger = VK_NULL_HANDLE;
    }

    void createInstance() {
        if (glfwVulkanSupported() != GLFW_TRUE) {
            throw std::runtime_error("GLFW could not find a Vulkan loader");
        }
        auto appInfo = vkStructure<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        appInfo.pApplicationName = "MC Rebedrock";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 2, 0);
        appInfo.pEngineName = "MC Rebedrock";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 2, 0);
        appInfo.apiVersion = VK_API_VERSION_1_2;

        std::uint32_t extensionCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&extensionCount);
        if (glfwExtensions == nullptr) {
            throw std::runtime_error("GLFW did not provide Vulkan surface extensions");
        }
        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + extensionCount);
        VkInstanceCreateFlags flags = 0;
        if (instanceExtensionAvailable(kPortabilityEnumeration)) {
            extensions.push_back(kPortabilityEnumeration);
            flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
        validationEnabled = kRequestValidation && validationLayerAvailable();
        if (kRequestValidation && !validationEnabled) {
            std::cerr << "Warning: VK_LAYER_KHRONOS_validation is not installed.\n";
        }
        if (validationEnabled) {
            if (!instanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
                throw std::runtime_error("Validation requested but VK_EXT_debug_utils is missing");
            }
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        auto createInfo = vkStructure<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        createInfo.flags = flags;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        const char* validationLayers[]{kValidationLayer};
        auto messengerInfo = debugMessengerInfo();
        if (validationEnabled) {
            createInfo.enabledLayerCount = 1;
            createInfo.ppEnabledLayerNames = validationLayers;
            createInfo.pNext = &messengerInfo;
        }
        checkVk(vkCreateInstance(&createInfo, nullptr, &instance), "vkCreateInstance");
        createDebugMessenger();
        std::cout << "Vulkan validation: " << (validationEnabled ? "enabled" : "disabled") << '\n';
    }

    [[nodiscard]] QueueFamilyIndices findQueueFamilies(VkPhysicalDevice candidate) const {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
        QueueFamilyIndices indices;
        for (std::uint32_t index = 0; index < count; ++index) {
            if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
                indices.graphics = index;
            }
            VkBool32 presentSupported = VK_FALSE;
            checkVk(
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface, &presentSupported),
                "vkGetPhysicalDeviceSurfaceSupportKHR");
            if (presentSupported == VK_TRUE) {
                indices.present = index;
            }
            if (indices.complete()) {
                break;
            }
        }
        return indices;
    }

    [[nodiscard]] std::vector<VkExtensionProperties>
    deviceExtensions(VkPhysicalDevice candidate) const {
        std::uint32_t count = 0;
        checkVk(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr),
                "vkEnumerateDeviceExtensionProperties");
        std::vector<VkExtensionProperties> extensions(count);
        checkVk(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, extensions.data()),
                "vkEnumerateDeviceExtensionProperties");
        return extensions;
    }

    [[nodiscard]] bool hasDeviceExtension(const std::vector<VkExtensionProperties>& extensions,
                                          const char* wanted) const {
        return std::ranges::any_of(extensions, [wanted](const auto& extension) {
            return std::strcmp(extension.extensionName, wanted) == 0;
        });
    }

    [[nodiscard]] SwapchainSupport querySwapchain(VkPhysicalDevice candidate) const {
        SwapchainSupport support;
        checkVk(
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(candidate, surface, &support.capabilities),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        std::uint32_t formatCount = 0;
        checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface, &formatCount, nullptr),
                "vkGetPhysicalDeviceSurfaceFormatsKHR");
        support.formats.resize(formatCount);
        if (formatCount > 0U) {
            checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface, &formatCount,
                                                         support.formats.data()),
                    "vkGetPhysicalDeviceSurfaceFormatsKHR");
        }
        std::uint32_t presentCount = 0;
        checkVk(
            vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, surface, &presentCount, nullptr),
            "vkGetPhysicalDeviceSurfacePresentModesKHR");
        support.presentModes.resize(presentCount);
        if (presentCount > 0U) {
            checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, surface, &presentCount,
                                                              support.presentModes.data()),
                    "vkGetPhysicalDeviceSurfacePresentModesKHR");
        }
        return support;
    }

    [[nodiscard]] bool suitable(VkPhysicalDevice candidate) const {
        const auto indices = findQueueFamilies(candidate);
        const auto extensions = deviceExtensions(candidate);
        if (!indices.complete() ||
            !hasDeviceExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            return false;
        }
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (VK_VERSION_MAJOR(properties.apiVersion) < 1U ||
            (VK_VERSION_MAJOR(properties.apiVersion) == 1U &&
             VK_VERSION_MINOR(properties.apiVersion) < 2U)) {
            return false;
        }
        const auto support = querySwapchain(candidate);
        return !support.formats.empty() && !support.presentModes.empty();
    }

    void pickPhysicalDevice() {
        std::uint32_t count = 0;
        checkVk(vkEnumeratePhysicalDevices(instance, &count, nullptr),
                "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> candidates(count);
        checkVk(vkEnumeratePhysicalDevices(instance, &count, candidates.data()),
                "vkEnumeratePhysicalDevices");
        int bestScore = std::numeric_limits<int>::min();
        for (const auto candidate : candidates) {
            if (!suitable(candidate)) {
                continue;
            }
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            int score = static_cast<int>(properties.limits.maxImageDimension2D);
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                score += 100'000;
            }
            if (score > bestScore) {
                bestScore = score;
                physicalDevice = candidate;
            }
        }
        if (physicalDevice == VK_NULL_HANDLE) {
            throw std::runtime_error("No Vulkan 1.2 GPU supports the required swapchain features");
        }
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        VkPhysicalDeviceFeatures supportedFeatures{};
        vkGetPhysicalDeviceFeatures(physicalDevice, &supportedFeatures);
        samplerAnisotropySupported = supportedFeatures.samplerAnisotropy == VK_TRUE;
        maximumSamplerAnisotropy = properties.limits.maxSamplerAnisotropy;
        const auto supportedSamples = properties.limits.framebufferColorSampleCounts &
                                      properties.limits.framebufferDepthSampleCounts;
        // 上限压到 2x，因为当前 MoltenVK 不把 transient attachment 映射到片上内存
        // 在 2 倍分辨率帧缓冲上开 4x MSAA 要吃掉约 425 MB 真实显存
        // 而收益为零：本渲染器是顶点瓶颈，从来不是填充率瓶颈
        // 2x 用一半的内存拿到同样的边缘平滑
        maximumMsaaSamples = (supportedSamples & VK_SAMPLE_COUNT_2_BIT) != 0U
                                 ? VK_SAMPLE_COUNT_2_BIT
                                 : VK_SAMPLE_COUNT_1_BIT;
        std::cout << "Vulkan GPU: " << properties.deviceName << '\n';
    }

    void createLogicalDevice() {
        queueFamilies = findQueueFamilies(physicalDevice);
        const std::set<std::uint32_t> uniqueFamilies{queueFamilies.graphics.value(),
                                                     queueFamilies.present.value()};
        constexpr float priority = 1.0F;
        std::vector<VkDeviceQueueCreateInfo> queueInfos;
        for (const auto family : uniqueFamilies) {
            auto info =
                vkStructure<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
            info.queueFamilyIndex = family;
            info.queueCount = 1;
            info.pQueuePriorities = &priority;
            queueInfos.push_back(info);
        }
        std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        const auto available = deviceExtensions(physicalDevice);
        if (hasDeviceExtension(available, kPortabilitySubset)) {
            extensions.push_back(kPortabilitySubset);
        }
        VkPhysicalDeviceFeatures features{};
        features.samplerAnisotropy = samplerAnisotropySupported ? VK_TRUE : VK_FALSE;
        // 渲染器平时只需要"可见/不可见"
        // 原生 Vulkan 为诊断场景保留精确计数
        // Apple 上只有强制打开那条默认关闭的 MoltenVK 路径时才用 Boolean 可见性
#if defined(__APPLE__)
        features.occlusionQueryPrecise = VK_FALSE;
#else
        features.occlusionQueryPrecise = VK_TRUE;
#endif
        auto createInfo = vkStructure<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
        createInfo.pQueueCreateInfos = queueInfos.data();
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.pEnabledFeatures = &features;
        checkVk(vkCreateDevice(physicalDevice, &createInfo, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, queueFamilies.graphics.value(), 0, &graphicsQueue);
        vkGetDeviceQueue(device, queueFamilies.present.value(), 0, &presentQueue);
    }

    void createAllocator() {
        VmaAllocatorCreateInfo info{};
        info.instance = instance;
        info.physicalDevice = physicalDevice;
        info.device = device;
        info.vulkanApiVersion = VK_API_VERSION_1_2;
        // 默认 256 MB 的块远大于本渲染器的任何单次分配，块内大部分是空档且永远还不回去
        // 改成 32 MB 让池的粒度贴近工作集，空出来的块才能真正还给驱动
        info.preferredLargeHeapBlockSize = 32U * 1024U * 1024U;
        checkVk(vmaCreateAllocator(&info, &allocator), "vmaCreateAllocator");
    }

    void createCommandPool() {
        auto info =
            vkStructure<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        info.flags =
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        info.queueFamilyIndex = queueFamilies.graphics.value();
        checkVk(vkCreateCommandPool(device, &info, nullptr, &commandPool), "vkCreateCommandPool");
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    bool samplerAnisotropySupported = false;
    float maximumSamplerAnisotropy = 1.0F;
    VkSampleCountFlagBits maximumMsaaSamples = VK_SAMPLE_COUNT_1_BIT;
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    QueueFamilyIndices queueFamilies;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    bool validationEnabled = false;
};

} // namespace mc::render
