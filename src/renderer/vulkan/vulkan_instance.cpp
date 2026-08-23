#include "renderer/vulkan/vulkan_instance.h"

#include "platform/window.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace engine {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT /*severity*/,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* /*userData*/) {
    std::fprintf(stderr, "[vulkan validation] %s\n", callbackData->pMessage);
    return VK_FALSE;
}

} // namespace

VulkanInstance::VulkanInstance(Window& window, bool enableValidation)
    : window_(window) {
#ifndef NDEBUG
    validationEnabled_ = enableValidation && checkValidationLayerSupport();
    if (enableValidation && !validationEnabled_) {
        std::fputs("Warning: validation layers requested but unavailable; continuing without them\n", stderr);
    }
#else
    (void)enableValidation;
#endif

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Game Engine";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.pEngineName = "Custom Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    // Required extensions: GLFW's platform surface extension.
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
#ifdef NDEBUG
    constexpr bool kDebug = false;
#else
    constexpr bool kDebug = true;
#endif
    if (kDebug) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    const char* validationLayerName = "VK_LAYER_KHRONOS_validation";
#ifdef NDEBUG
    // Release build: no layers.
#else
    if (validationEnabled_) {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = &validationLayerName;
    }
#endif

    if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
        std::fputs("Fatal: vkCreateInstance failed\n", stderr);
        std::exit(EXIT_FAILURE);
    }

#ifdef NDEBUG
    // No debug messenger in release builds.
#else
    if (validationEnabled_) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (!create) {
            std::fputs("Fatal: VK_EXT_debug_utils not supported\n", stderr);
            std::exit(EXIT_FAILURE);
        }

        VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
        messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messengerInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messengerInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messengerInfo.pfnUserCallback = debugCallback;

        if (create(instance_, &messengerInfo, nullptr, &debugMessenger_) != VK_SUCCESS) {
            std::fputs("Fatal: failed to set up debug messenger\n", stderr);
            std::exit(EXIT_FAILURE);
        }
    }
#endif

    // Surface created after instance, via GLFW.
    if (glfwCreateWindowSurface(instance_, window_.handle(), nullptr, &surface_) != VK_SUCCESS) {
        std::fputs("Fatal: failed to create window surface\n", stderr);
        std::exit(EXIT_FAILURE);
    }
}

VulkanInstance::~VulkanInstance() {
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
#ifdef NDEBUG
    // No messenger cleanup needed.
#else
    if (debugMessenger_ != VK_NULL_HANDLE) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) {
            destroy(instance_, debugMessenger_, nullptr);
        }
    }
#endif
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

bool VulkanInstance::checkValidationLayerSupport() const {
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    for (const auto& layer : availableLayers) {
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
            return true;
        }
    }
    return false;
}

} // namespace engine
