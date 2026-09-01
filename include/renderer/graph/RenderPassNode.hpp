#pragma once
#include "renderer/scheduler/QueueTypes.hpp"
#include "RenderGraphResources.hpp"
#include <functional>
#include <vector>
#include <string>
#include <vulkan/vulkan.h>

namespace Engine {

struct RenderPassNode {
    std::string name;
    uint32_t passIndex{0};
    QueueType preferredQueue{QueueType::Graphics}; // Target execution lane
    QueueType actualQueue{QueueType::Graphics};    // Resolved execution lane after fallback check
    
    std::vector<std::pair<ResourceHandle, ResourceUsage>> reads;
    std::vector<std::pair<ResourceHandle, ResourceUsage>> writes;
    std::vector<std::pair<BufferHandle, BufferUsage>> bufferReads;
    std::vector<std::pair<BufferHandle, BufferUsage>> bufferWrites;
    std::function<void(VkCommandBuffer)> executeCallback;
};

} // namespace Engine

namespace engine {
    using RenderPassNode = Engine::RenderPassNode;
}
