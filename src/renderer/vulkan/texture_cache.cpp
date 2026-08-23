#include "renderer/vulkan/texture_cache.h"

#include "renderer/vulkan/vk_device.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <functional>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <limits.h>

namespace engine {

namespace {

void fatal(const char* msg) {
    std::fprintf(stderr, "Fatal: %s\n", msg);
    std::exit(EXIT_FAILURE);
}

uint32_t findMemoryType(VkPhysicalDevice physical, uint32_t typeFilter,
                        VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    fatal("failed to find suitable memory type");
    return 0;
}

// One-shot copy command on the graphics queue.
void submitCopy(const VulkanDevice& device, const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = device.commandPool();
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device.handle(), &allocInfo, &cmd) != VK_SUCCESS)
        fatal("failed to allocate upload command buffer");

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    record(cmd);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(device.graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(device.graphicsQueue());
    vkFreeCommandBuffers(device.handle(), device.commandPool(), 1, &cmd);
}

} // namespace

TextureCache::~TextureCache() {
    VkDevice dev = device_.handle();
    for (auto* t : textures_) {
        if (t->sampler) vkDestroySampler(dev, t->sampler, nullptr);
        if (t->view) vkDestroyImageView(dev, t->view, nullptr);
        if (t->image) vkDestroyImage(dev, t->image, nullptr);
        if (t->memory) vkFreeMemory(dev, t->memory, nullptr);
        delete t;
    }
}

const Texture* TextureCache::load(const std::string& path) {
    auto it = cache_.find(path);
    if (it != cache_.end()) return it->second;

    // Resolve relative to executable dir.
    char buf[PATH_MAX];
    ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string exeDir = len > 0 ? std::string(buf, static_cast<size_t>(len)) : ".";
    size_t slash = exeDir.find_last_of('/');
    exeDir = slash == std::string::npos ? "." : exeDir.substr(0, slash);

    std::string full = path;
    for (const std::string& base : {exeDir + "/", exeDir + "/../"}) {
        std::FILE* probe = std::fopen((base + path).c_str(), "rb");
        if (probe) {
            std::fclose(probe);
            full = base + path;
            break;
        }
    }

    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load(full.c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels) {
        std::fprintf(stderr, "[texture] failed to load %s\n", full.c_str());
        return nullptr;
    }
    const Texture* tex = upload(path, pixels, w, h);
    stbi_image_free(pixels);
    cache_[path] = tex;
    return tex;
}

const Texture* TextureCache::createFromPixels(const std::string& key, const unsigned char* rgba,
                                              int width, int height) {
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    const Texture* tex = upload(key, rgba, width, height);
    cache_[key] = tex;
    return tex;
}

const Texture* TextureCache::upload(const std::string& key, const unsigned char* rgba,
                                    int width, int height) {
    VkDevice dev = device_.handle();
    VkPhysicalDevice physical = device_.physical();
    VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4;

    // Staging buffer (host-visible).
    VkBuffer staging;
    VkDeviceMemory stagingMem;
    {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = imageSize;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(dev, &bufInfo, nullptr, &staging) != VK_SUCCESS)
            fatal("failed to create texture staging buffer");

        VkMemoryRequirements reqs{};
        vkGetBufferMemoryRequirements(dev, staging, &reqs);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = reqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(
            physical, reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(dev, &allocInfo, nullptr, &stagingMem) != VK_SUCCESS)
            fatal("failed to allocate texture staging memory");
        if (vkBindBufferMemory(dev, staging, stagingMem, 0) != VK_SUCCESS)
            fatal("failed to bind texture staging buffer");

        void* data = nullptr;
        vkMapMemory(dev, stagingMem, 0, imageSize, 0, &data);
        std::memcpy(data, rgba, static_cast<size_t>(imageSize));
        vkUnmapMemory(dev, stagingMem);
    }

    auto* tex = new Texture();
    tex->width = static_cast<uint32_t>(width);
    tex->height = static_cast<uint32_t>(height);

    // GPU image.
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imageInfo.extent = {tex->width, tex->height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &imageInfo, nullptr, &tex->image) != VK_SUCCESS)
        fatal("failed to create texture image");

    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(dev, tex->image, &reqs);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = reqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        physical, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev, &allocInfo, nullptr, &tex->memory) != VK_SUCCESS)
        fatal("failed to allocate texture memory");
    if (vkBindImageMemory(dev, tex->image, tex->memory, 0) != VK_SUCCESS)
        fatal("failed to bind texture image");

    // Upload + layout transitions in one submission:
    // UNDEFINED -> TRANSFER_DST, copy, -> SHADER_READ_ONLY.
    submitCopy(device_, [&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.image = tex->image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &barrier);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {tex->width, tex->height, 1};
        vkCmdCopyBufferToImage(cmd, staging, tex->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &barrier);
    });

    vkDestroyBuffer(dev, staging, nullptr);
    vkFreeMemory(dev, stagingMem, nullptr);

    // View.
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = tex->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(dev, &viewInfo, nullptr, &tex->view) != VK_SUCCESS)
        fatal("failed to create texture view");

    // Sampler: linear filtering, repeat wrap.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(dev, &samplerInfo, nullptr, &tex->sampler) != VK_SUCCESS)
        fatal("failed to create texture sampler");

    textures_.push_back(tex);
    std::printf("[texture] loaded %s (%ux%u)\n", key.c_str(), tex->width, tex->height);
    return tex;
}

} // namespace engine
