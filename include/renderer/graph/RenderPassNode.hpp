#pragma once
#include "RenderGraphResources.hpp"
#include <functional>
#include <vector>
#include <string>
#include <vulkan/vulkan.h>

namespace Engine {

struct RenderPassNode {
    std::string name;
    uint32_t passIndex{0};
    std::vector<std::pair<ResourceHandle, ResourceUsage>> reads;
    std::vector<std::pair<ResourceHandle, ResourceUsage>> writes;
    std::function<void(VkCommandBuffer)> executeCallback;
};

} // namespace Engine

namespace engine {
    using RenderPassNode = Engine::RenderPassNode;
}
