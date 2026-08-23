#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <string>
#include <unordered_map>
#include <vector>

// GPU texture: image + view + sampler. Owned by TextureCache.
namespace engine {

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
};

class VulkanDevice;

class TextureCache {
public:
    explicit TextureCache(const VulkanDevice& device) : device_(device) {}
    ~TextureCache();

    // Loads RGBA8 image from disk, uploads via staging, creates view+sampleer.
    // Returns nullptr on failure (caller falls back to material color).
    const Texture* load(const std::string& path);
    // Wraps an existing CPU texture (e.g. generated checkerboard). Takes ownership of pixels.
    const Texture* createFromPixels(const std::string& key, const unsigned char* rgba,
                                    int width, int height);

private:
    const Texture* upload(const std::string& key, const unsigned char* rgba,
                          int width, int height);

    const VulkanDevice& device_;
    std::vector<Texture*> textures_;
    std::unordered_map<std::string, const Texture*> cache_;
};

} // namespace engine
