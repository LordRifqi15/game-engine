#include "renderer/graph/TransientResourcePool.hpp"
#include "renderer/vulkan/VkDeviceMemoryHelper.hpp"

namespace Engine {

TransientResourcePool::TransientResourcePool(VkDevice device, VkPhysicalDevice physicalDevice)
    : device_(device), physicalDevice_(physicalDevice) {}

TransientResourcePool::~TransientResourcePool() {
    clear();
}

bool TransientResourcePool::isCompatible(const PhysicalImage& physical, const ImageDesc& desc) const {
    return physical.format == desc.format &&
           physical.extent.width == desc.extent.width &&
           physical.extent.height == desc.extent.height &&
           (physical.usage & desc.usage) == desc.usage;
}

PhysicalImage* TransientResourcePool::createPhysicalImage(const ImageDesc& desc) {
    auto* phys = new PhysicalImage();
    phys->format = desc.format;
    phys->extent = desc.extent;
    phys->usage = desc.usage;
    phys->currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    phys->lastUsedPassIndex = UINT32_MAX;
    phys->lastUsedFrameIndex = 0;
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    // Use helper; handles dummy when device null
    createImageWithMemory(device_, physicalDevice_, desc.format, desc.extent, desc.usage, &img, &mem, &view);
    phys->image = img;
    phys->memory = mem;
    phys->view = view;
    return phys;
}

PhysicalImage* TransientResourcePool::acquireImage(const ImageDesc& desc, uint32_t passIndex, uint64_t frameIndex) {
    // default to single-pass lifetime (first == last == passIndex)
    return acquireImage(desc, passIndex, passIndex, frameIndex);
}

PhysicalImage* TransientResourcePool::acquireImage(const ImageDesc& desc, uint32_t firstPass, uint32_t lastPass, uint64_t frameIndex) {
    auto& activePool = framePools_[frameIndex % MAX_FRAMES_IN_FLIGHT];
    // 1. Search reusable whose lifetime ended before this pass OR from different frame
    for (auto& entry : activePool) {
        if (isCompatible(*entry, desc)) {
            bool sameFrame = (entry->lastUsedFrameIndex == frameIndex);
            bool canReuse = false;
            if (entry->lastUsedPassIndex == UINT32_MAX) canReuse = true;
            else if (!sameFrame) canReuse = true; // different frame in same slot, safe to reuse (double buffering)
            else if (entry->lastUsedPassIndex < firstPass) canReuse = true;
            if (canReuse) {
                entry->lastUsedPassIndex = lastPass;
                entry->lastUsedFrameIndex = frameIndex;
                return entry.get();
            }
        }
    }
    // 2. allocate new
    auto* newImage = createPhysicalImage(desc);
    newImage->lastUsedPassIndex = lastPass;
    newImage->lastUsedFrameIndex = frameIndex;
    activePool.push_back(std::unique_ptr<PhysicalImage>(newImage));
    return activePool.back().get();
}

void TransientResourcePool::advanceFrame(uint64_t currentFrameIndex) {
    currentFrameSlot_ = currentFrameIndex % MAX_FRAMES_IN_FLIGHT;
    for (auto& entry : framePools_[currentFrameSlot_]) {
        entry->lastUsedPassIndex = UINT32_MAX; // reset for recycling; will be overwritten on next acquire
        // keep frame index as is for debugging
    }
}

void TransientResourcePool::clear() {
    for (int i = 0; i < (int)MAX_FRAMES_IN_FLIGHT; ++i) {
        for (auto& p : framePools_[i]) {
            if (device_ != VK_NULL_HANDLE) {
                if (p->view) vkDestroyImageView(device_, p->view, nullptr);
                if (p->image) vkDestroyImage(device_, p->image, nullptr);
                if (p->memory) vkFreeMemory(device_, p->memory, nullptr);
            }
        }
        framePools_[i].clear();
    }
}

size_t TransientResourcePool::getTotalAllocationCount() const {
    size_t n = 0;
    for (int i = 0; i < (int)MAX_FRAMES_IN_FLIGHT; ++i) n += framePools_[i].size();
    return n;
}

size_t TransientResourcePool::getFrameAllocationCount(uint32_t frameSlot) const {
    if (frameSlot >= MAX_FRAMES_IN_FLIGHT) return 0;
    return framePools_[frameSlot].size();
}

} // namespace Engine
