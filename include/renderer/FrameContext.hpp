#pragma once
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>
#include "renderer/lighting/LightTypes.hpp"
#include "renderer/meshlet/MeshletTypes.hpp"

namespace Engine {

struct CameraSnapshot {
    glm::mat4 viewMatrix;
    glm::mat4 projMatrix;
    glm::mat4 invViewProj;
    glm::vec3 worldPosition;
    float zNear;
    float zFar;
    float fov;
    float aspectRatio;
};

struct FrameContext {
    uint64_t frameIndex{0};
    uint32_t frameSlot{0}; // frameIndex % MAX_FRAMES_IN_FLIGHT
    float dt{0.0f};

    CameraSnapshot camera;
    VkExtent2D renderExtent{0, 0};
    uint32_t swapchainImageIndex{0};

    // GPU Resource Buffers
    VkBuffer globalVertexBuffer{VK_NULL_HANDLE};
    VkBuffer globalIndexBuffer{VK_NULL_HANDLE};
    VkBuffer globalInstanceBuffer{VK_NULL_HANDLE};
    VkBuffer globalMeshletBuffer{VK_NULL_HANDLE};
    VkBuffer globalMaterialBuffer{VK_NULL_HANDLE};
    VkBuffer globalLightBuffer{VK_NULL_HANDLE};

    // Lighting & Geometry Metadata
    uint32_t totalInstances{0};
    uint32_t totalMeshlets{0};
    uint32_t activeLightCount{0};
    ClusterUniforms clusterUniforms;

    // Output target handles
    VkImage swapchainImage{VK_NULL_HANDLE};
    VkImageView swapchainImageView{VK_NULL_HANDLE};
    VkFormat swapchainFormat{VK_FORMAT_B8G8R8A8_UNORM};
};

} // namespace Engine

namespace engine {
    using FrameContext = ::Engine::FrameContext;
    using CameraSnapshot = ::Engine::CameraSnapshot;
}
