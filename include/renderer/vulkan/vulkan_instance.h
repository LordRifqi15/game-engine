#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace engine {

class Window;

// Owns VkInstance and (debug builds) the debug messenger.
// Also owns the window surface via GLFW.
class VulkanInstance {
public:
    VulkanInstance(Window& window, bool enableValidation = true);
    ~VulkanInstance();

    VulkanInstance(const VulkanInstance&) = delete;
    VulkanInstance& operator=(const VulkanInstance&) = delete;

    VkInstance handle() const { return instance_; }
    VkSurfaceKHR surface() const { return surface_; }
    bool validationEnabled() const { return validationEnabled_; }

private:
    bool checkValidationLayerSupport() const;

    Window& window_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    bool validationEnabled_ = false;

#ifndef NDEBUG
    static constexpr bool kDefaultValidation = true;
#else
    static constexpr bool kDefaultValidation = false;
#endif

public:
    // Expose resolved default so Application can pass it through cleanly.
    static constexpr bool wantValidationByDefault() { return kDefaultValidation; }
};

} // namespace engine
