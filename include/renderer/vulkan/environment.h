#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <array>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

class VulkanDevice;
class VulkanSwapchain;

// Procedural sky environment + precomputed IBL maps (irradiance, prefiltered
// mips, BRDF LUT) and the skybox draw. Everything is generated once at startup.
class VulkanEnvironment {
public:
    VulkanEnvironment(VulkanDevice& device, const VulkanSwapchain& swapchain,
                      VkDescriptorSetLayout cameraLayout,
                      VkDescriptorSetLayout materialLayout,
                      VkDescriptorSetLayout shadowSamplerLayout);
    ~VulkanEnvironment();

    VulkanEnvironment(const VulkanEnvironment&) = delete;
    VulkanEnvironment& operator=(const VulkanEnvironment&) = delete;

    // Set 3 layout: 0 = irradiance, 1 = prefiltered, 2 = BRDF LUT, 3 = env cube.
    VkDescriptorSetLayout set3Layout() const { return set3Layout_; }
    // One set per frame-in-flight, already written.
    VkDescriptorSet frameSet(uint32_t frameIndex) const {
        return frameSets_[frameIndex];
    }

    // Skybox: binds its own pipeline + env descriptor, draws the unit cube.
    void beginSkybox(VkCommandBuffer cmd, uint32_t frameIndex,
                     const glm::mat4& viewProjection, const glm::vec3& cameraPos);
    void drawSkybox(VkCommandBuffer cmd);
    void endSkybox(VkCommandBuffer cmd);

private:
    struct CubeTexture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t mipLevels = 1;
    };

    void generateEnvPixels(std::vector<std::vector<unsigned char>>& faces,
                           uint32_t faceSize);
    void buildCubemaps();
    void buildBrdfLut();
    void createSamplersAndSet3();
    void buildSkyboxPipelineAndMesh();
    void createSkyboxPipeline(VkRenderPass renderPass,
                              VkDescriptorSetLayout cameraLayout,
                              VkDescriptorSetLayout materialLayout,
                              VkDescriptorSetLayout shadowSamplerLayout);
    void createCubeImage(CubeTexture& out, uint32_t size, uint32_t mipLevels,
                         VkFormat format, const std::vector<std::vector<unsigned char>>& facesRGBA8,
                         const std::vector<std::vector<float>>* facesHDR16 = nullptr);
    void uploadCubeFaces(CubeTexture& tex, uint32_t size, uint32_t mipLevels,
                         const std::vector<std::vector<unsigned char>>& facesRGBA8,
                         const std::vector<std::vector<float>>* facesHDR16);

    static glm::vec3 cubeDirection(uint32_t face, uint32_t x, uint32_t y, uint32_t size);

    VulkanDevice& device_;
    const VulkanSwapchain& swapchain_;

    CubeTexture env_{};
    CubeTexture irradiance_{};
    CubeTexture prefiltered_{};

    VkImageView brdfView_ = VK_NULL_HANDLE;
    VkDeviceMemory brdfMemory_ = VK_NULL_HANDLE;
    VkImage brdfImage_ = VK_NULL_HANDLE;

    VkSampler envSampler_ = VK_NULL_HANDLE;      // for skybox
    VkSampler irrSampler_ = VK_NULL_HANDLE;      // linear clamp
    VkSampler prefSampler_ = VK_NULL_HANDLE;     // trilinear mips
    VkSampler lutSampler_ = VK_NULL_HANDLE;      // linear clamp edge

    VkDescriptorSetLayout set3Layout_ = VK_NULL_HANDLE;
    VkDescriptorPool set3Pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> frameSets_; // per frame-in-flight

    // Skybox resources.
    VkPipeline skyboxPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout skyboxPipelineLayout_ = VK_NULL_HANDLE;
    VkBuffer skyboxVertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory skyboxVertexMemory_ = VK_NULL_HANDLE;
    VkBuffer skyboxIndexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory skyboxIndexMemory_ = VK_NULL_HANDLE;

    std::array<VkDescriptorSetLayout, 3> otherSetLayouts_{};
    std::vector<glm::vec3> sunDirs_; // debug hook (unused at runtime)
};

} // namespace engine
