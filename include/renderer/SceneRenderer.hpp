#pragma once
// Task 053: authoritative modern scene rendering. Owns GPU upload buffers,
// bindless materials, compute + graphics pipelines, and descriptor sets for
// the RenderGraph live path. Reuses legacy VulkanDevice/VulkanSwapchain,
// TextureCache, shadow maps, and IBL cubemaps (borrowed, never owned).
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "renderer/GPUScene.hpp"
#include "renderer/FrameContext.hpp"
#include "renderer/graph/RenderGraph.hpp"
#include "renderer/lighting/LightTypes.hpp"
#include "renderer/material/BindlessDescriptorManager.hpp"
#include "renderer/meshlet/MeshletTypes.hpp"
#include "renderer/deferred/GBuffer.hpp"

namespace engine {
 class Mesh;
class Texture;
class VulkanDevice;
class VulkanSwapchain;
class TextureCache;
class VulkanShadowPass;
class VulkanEnvironment;
} // namespace engine

namespace Engine {

struct SceneRenderStats {
    uint32_t instances{0};
    uint32_t staticDraws{0};
    uint32_t skinnedDraws{0};
    uint32_t meshlets{0};
    uint32_t lights{0};
    uint32_t clusters{0};
    uint32_t draws{0};
    uint32_t triangles{0};
    uint32_t residentPages{0};
    uint32_t totalPages{0};
    uint32_t pageRequests{0};
};

class SceneRenderer {
public:
    static constexpr uint32_t kFrames = 2;
    static constexpr uint32_t kMaxDraws = 4096;
    static constexpr uint32_t kMaxJoints = 128;
    static constexpr uint32_t kMaxLights = 256;
    static constexpr uint32_t kClusterZ = 24;
    static constexpr uint32_t kGridXCap = 64;
    static constexpr uint32_t kGridYCap = 36;
    static constexpr uint32_t kIndexListCap = 262144;
    static constexpr uint32_t kCompactedCap = 4194304;
    static constexpr uint32_t kMaxPages = 4096;
    static constexpr uint32_t kRequestCap = 1024;

    SceneRenderer() = default;
    ~SceneRenderer() { shutdown(); }

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    bool init(VkDevice device, VkPhysicalDevice physical, VkQueue graphicsQueue,
              uint32_t graphicsFamily, VkFormat swapchainFormat,
              ::engine::VulkanDevice& legacyDevice, ::engine::VulkanSwapchain& swapchain,
              ::engine::TextureCache& textures, ::engine::VulkanEnvironment& environment);
    void shutdown();
    bool ready() const { return ready_; }

    // CPU snapshot upload into mapped rings; bakes static meshlets when dirty.
    void upload(const GPUScene& scene, const FrameContext& ctx, uint32_t frameSlot);

    // Registers the real 10-pass workload on the graph.
    void buildPasses(RenderGraph& graph, const FrameContext& ctx);

    void setDebugView(int v) { debugView_ = v; }
    int debugView() const { return debugView_; }
    void setExposure(float e) { exposure_ = e; }
    // Debug-only HDR capture (ENGINE_READBACK=1). Stalls the GPU; never hot.
    void debugReadback(const char* path);
    void debugReadbackDepth(const char* path);
    const SceneRenderStats& stats() const { return stats_; }
    VkImage lastHdrImage_{VK_NULL_HANDLE};
    VkExtent2D lastHdrExtent_{0, 0};

private:
    // ---- buffers ----
    struct HostBuffer {
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        void* mapped{nullptr};
        VkDeviceSize size{0};
    };
    bool createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, HostBuffer& out);
    void destroyHostBuffer(HostBuffer& buf);
    void ensureStaticCapacity(VkDeviceSize verts, VkDeviceSize meshlets, VkDeviceSize uniq,
                              VkDeviceSize tris, uint32_t pages);

    struct MeshBuffers {
        HostBuffer verts;
        HostBuffer indices;
        uint32_t indexCount{0};
    };
    MeshBuffers& meshBuffers(const ::engine::Mesh* mesh);

    // ---- bake ----
    void bakeStatics(const GPUScene& scene);
    bool staticsDirty(const GPUScene& scene) const;
    std::vector<glm::mat4> lastStaticModels_;

    // ---- descriptors/pipelines ----
    bool createDescriptorPool();
    bool createLayouts();
    bool createStaticDescriptors();
    void writeFrameDescriptors(uint32_t frameSlot);
    bool createPipelines(VkFormat swapchainFormat);
    bool createSamplers();
    VkShaderModule loadShader(const char* name);
    void destroyPipelines();
    void destroyDescriptors();

    // ---- records (called from graph pass callbacks) ----
    void recordShadow(VkCommandBuffer cb, const FrameContext& ctx);
    void recordClusterCull(VkCommandBuffer cb, const FrameContext& ctx);
    void recordHiZBuild(VkCommandBuffer cb, const FrameContext& ctx);
    void recordMeshletCull(VkCommandBuffer cb, const FrameContext& ctx);
    void recordGBuffer(VkCommandBuffer cb, const FrameContext& ctx);
    void recordDeferred(VkCommandBuffer cb, const FrameContext& ctx);
    void recordForward(VkCommandBuffer cb, const FrameContext& ctx);
    void recordTonemap(VkCommandBuffer cb, const FrameContext& ctx);
    void beginTarget(VkCommandBuffer cb, std::initializer_list<VkRenderingAttachmentInfo> colors,
                     const VkRenderingAttachmentInfo* depth, VkExtent2D extent);
    // ---- graph resources needed by records (filled in buildPasses) ----
    struct FrameGraphRefs {
        ResourceHandle hiz;
        ResourceHandle hdr;
        ResourceHandle historyDepth;
        ResourceHandle historyRead;
        GBufferHandles gbuffer{};
        BufferHandle lightBuffer;
        BufferHandle clusterGrid;
        BufferHandle clusterIndex;
        BufferHandle meshletBuffer;
        BufferHandle compacted;
        BufferHandle indirect;
        BufferHandle residency;
        BufferHandle requests;
        BufferHandle vertexPool;
        BufferHandle uniquePool;
        BufferHandle triPool;
        VkExtent2D hizExtent{0, 0};
        uint32_t hizMips{1};
    } refs_;

    VkDevice device_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_{VK_NULL_HANDLE};
    VkQueue graphicsQueue_{VK_NULL_HANDLE};
    uint32_t graphicsFamily_{UINT32_MAX};
    VkFormat swapchainFormat_{VK_FORMAT_UNDEFINED};
    ::engine::VulkanDevice* legacyDevice_{nullptr};
    ::engine::VulkanSwapchain* legacySwapchain_{nullptr};
    ::engine::TextureCache* textures_{nullptr};
    ::engine::VulkanEnvironment* environment_{nullptr};
    ::engine::VulkanShadowPass* shadowPass_{nullptr};
    bool ready_{false};

    // Rings (per frame slot)
    HostBuffer instanceRings_[kFrames];
    HostBuffer jointRings_[kFrames];
    HostBuffer cameraRings_[kFrames];
    HostBuffer frameUboRings_[kFrames];
    HostBuffer lightRings_[kFrames];
    HostBuffer cullCamRings_[kFrames];
    HostBuffer meshletCamRings_[kFrames];
    // Working (per frame slot: frames in flight overlap, so singletons race)
    HostBuffer clusterGrid_[kFrames];
    HostBuffer clusterIndex_[kFrames];
    HostBuffer compacted_[kFrames];
    HostBuffer indirect_[kFrames];
    HostBuffer residency_[kFrames];
    HostBuffer requests_[kFrames];
    // Static pools (grown on demand)
    HostBuffer vertexPool_;
    HostBuffer meshletPool_;
    HostBuffer uniquePool_;
    HostBuffer triPool_;
    std::unordered_map<const ::engine::Mesh*, MeshBuffers> meshCache_;
    HostBuffer skyboxVerts_;
    HostBuffer skyboxIndices_;
    std::unordered_map<const ::engine::Texture*, uint32_t> textureSlots_;
    std::vector<uint32_t> drawMaterial_; // per-draw bindless id, parallel to GPUScene::draws
    std::unordered_map<uint64_t, uint32_t> materialSlots_; // content hash -> bindless material id
    uint32_t nextMaterial_{0};

    VkImage historyImages_[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory historyMemories_[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView historyViews_[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkExtent2D historyExtent_{0, 0};
    void ensureHistory(VkExtent2D extent);

    BindlessDescriptorManager bindless_;
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    VkDescriptorSet cameraSets_[kFrames]{};
    VkDescriptorSet jointSets_[kFrames]{};
    VkDescriptorSet frameUboSets_[kFrames]{};
    VkDescriptorSet cullSets_[kFrames]{};
    VkDescriptorSet meshletCullSets_[kFrames]{};
    VkDescriptorSet meshletGfxSets_[kFrames]{};
    VkDescriptorSet deferredGbufferSets_[kFrames]{};
    VkDescriptorSet deferredLightSets_[kFrames]{};
    VkDescriptorSet deferredFrameSets_[kFrames]{};
    VkDescriptorSet tonemapSets_[kFrames]{};
    VkDescriptorSetLayout cameraSetLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout jointSetLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout frameUboLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout emptyLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout cullLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout hizLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout meshletCullLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout meshletGfxLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout deferredGbufferLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout deferredLightLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout deferredFrameLayout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout tonemapLayout_{VK_NULL_HANDLE};
    VkDescriptorSet hizSets_[kFrames][12]{};
    VkSampler hizSampler_{VK_NULL_HANDLE};
    VkSampler gbufferSampler_{VK_NULL_HANDLE};

    VkPipelineLayout cullPipeLayout_{VK_NULL_HANDLE};
    VkPipeline cullPipeline_{VK_NULL_HANDLE};
    VkPipelineLayout hizPipeLayout_{VK_NULL_HANDLE};
    VkPipeline hizPipeline_{VK_NULL_HANDLE};
    VkPipelineLayout meshletCullPipeLayout_{VK_NULL_HANDLE};
    VkPipeline meshletCullPipeline_{VK_NULL_HANDLE};
    VkPipelineLayout gbufferPipeLayout_{VK_NULL_HANDLE};
    VkPipeline gbufferClassicPipeline_{VK_NULL_HANDLE};
    VkPipelineLayout gbufferMeshletPipeLayout_{VK_NULL_HANDLE};
    VkPipeline gbufferMeshletPipeline_{VK_NULL_HANDLE};
    VkPipelineLayout deferredPipeLayout_{VK_NULL_HANDLE};
    VkPipeline deferredPipeline_{VK_NULL_HANDLE};
    VkPipelineLayout tonemapPipeLayout_{VK_NULL_HANDLE};
    VkPipeline tonemapPipeline_{VK_NULL_HANDLE};
    VkPipelineLayout skyboxPipeLayout_{VK_NULL_HANDLE};
    VkPipeline skyboxPipeline_{VK_NULL_HANDLE};
    const GPUScene* scene_{nullptr};
    FrameContext ctx_{};
    uint32_t activeSlot_{0};
    RenderGraph* activeGraph_{nullptr};
    std::vector<VkImageView> hizViews_[kFrames];
    glm::mat4 shadowVPs_[4]{glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f)};
    int debugView_{0};
    float exposure_{1.0f};
    SceneRenderStats stats_{};
    uint32_t clusterCount_{0};
    uint32_t bakedMeshlets_{0};
    uint32_t bakedTris_{0};
    uint32_t bakedPages_{0};
};

} // namespace Engine
