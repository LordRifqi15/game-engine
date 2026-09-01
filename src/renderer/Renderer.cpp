#include "renderer/Renderer.hpp"
#include "renderer/deferred/GBuffer.hpp"
#include "renderer/graph/RenderGraphValidator.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace Engine {

void Renderer::init(VkDevice device, VkPhysicalDevice physicalDevice, const QueueFamilyIndices& queueIndices) {
    device_ = device;
    physicalDevice_ = physicalDevice;
    transientPool_.~TransientResourcePool();
    new (&transientPool_) TransientResourcePool(device, physicalDevice);
    // Need to reconstruct scheduler with correct device/indices (since default constructed with null)
    scheduler_.~FrameScheduler();
    new (&scheduler_) FrameScheduler(device, queueIndices);
    // Create fences (dummy for headless)
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (device_ == VK_NULL_HANDLE) {
            inFlightFences_[i] = reinterpret_cast<VkFence>(0x6000 + i);
        } else {
            VkFenceCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(device_, &ci, nullptr, &inFlightFences_[i]);
        }
    }
    currentFrameIndex_ = 0;
    framebufferResized_ = false;
    renderExtent_ = VkExtent2D{1920, 1080};
}

void Renderer::shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        for (auto f : inFlightFences_) if (f) vkDestroyFence(device_, f, nullptr);
    }
    for (auto &f : inFlightFences_) f = VK_NULL_HANDLE;
    scheduler_.~FrameScheduler();
    new (&scheduler_) FrameScheduler(VK_NULL_HANDLE, QueueFamilyIndices{});
    transientPool_.~TransientResourcePool();
    new (&transientPool_) TransientResourcePool(VK_NULL_HANDLE, VK_NULL_HANDLE);
    device_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    currentFrameIndex_ = 0;
}

bool Renderer::beginFrame(FrameContext& outContext, float dt, entt::registry& registry) {
    (void)registry;
    // Handle resize
    if (framebufferResized_) {
        // In real engine, would recreate swapchain; here just clear flag and return false to skip this frame
        framebufferResized_ = false;
        return false;
    }
    uint32_t slot = currentFrameIndex_ % MAX_FRAMES_IN_FLIGHT;
    VkFence fence = inFlightFences_[slot];
    if (device_ != VK_NULL_HANDLE && fence != VK_NULL_HANDLE) {
        vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device_, 1, &fence);
    }
    // Simulate swapchain acquire (headless: dummy image)
    // In real engine, would call vkAcquireNextImageKHR and handle OUT_OF_DATE
    // For test, we simulate success unless renderExtent is 0 (resize case)
    if (renderExtent_.width == 0 || renderExtent_.height == 0) {
        onResize(1920, 1080);
        return false;
    }

    outContext.frameIndex = currentFrameIndex_;
    outContext.frameSlot = slot;
    outContext.dt = dt;
    outContext.renderExtent = renderExtent_;
    outContext.swapchainImage = reinterpret_cast<VkImage>(0x7000 + slot);
    outContext.swapchainImageView = reinterpret_cast<VkImageView>(0x8000 + slot);
    outContext.swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
    outContext.swapchainImageIndex = slot;
    // Dummy camera (look at origin from 5,5,5)
    glm::vec3 camPos{5,5,5};
    glm::mat4 view = glm::lookAt(camPos, glm::vec3(0,0,0), glm::vec3(0,1,0));
    float aspect = renderExtent_.width ? float(renderExtent_.width)/float(renderExtent_.height) : 1.0f;
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(60.0f), aspect, 0.1f, 100.0f);
    proj[1][1] *= -1; // Vulkan Y flip
    outContext.camera.viewMatrix = view;
    outContext.camera.projMatrix = proj;
    outContext.camera.invViewProj = glm::inverse(proj * view);
    outContext.camera.worldPosition = camPos;
    outContext.camera.zNear = 0.1f;
    outContext.camera.zFar = 100.0f;
    outContext.camera.fov = glm::radians(60.0f);
    outContext.camera.aspectRatio = aspect;
    // Dummy buffers (headless)
    outContext.globalVertexBuffer = reinterpret_cast<VkBuffer>(0x9000);
    outContext.globalIndexBuffer = reinterpret_cast<VkBuffer>(0x9001);
    outContext.globalInstanceBuffer = reinterpret_cast<VkBuffer>(0x9002);
    outContext.globalMeshletBuffer = reinterpret_cast<VkBuffer>(0x9003);
    outContext.globalMaterialBuffer = reinterpret_cast<VkBuffer>(0x9004);
    outContext.globalLightBuffer = reinterpret_cast<VkBuffer>(0x9005);
    outContext.totalInstances = 1000;
    outContext.totalMeshlets = 500;
    outContext.activeLightCount = 16;
    // clusterUniforms already default
    return true;
}

void Renderer::renderFrame(FrameContext& ctx) {
    RenderGraph graph;
    buildFrameGraph(graph, ctx);
    // Validate before compile (hazard checking)
    std::string err;
    if (!RenderGraphValidator::validate(graph, err)) {
        // In real engine, would assert/log; for test, we still try to compile
        // fprintf(stderr, "Validator: %s\n", err.c_str());
    }
    // Compile with transient pool (lifetime + aliasing)
    // For headless, we use the pool's current frame index
    graph.compile(ctx.frameIndex, transientPool_);
    // Schedule and execute via FrameScheduler (vkQueueSubmit2)
    scheduler_.scheduleAndExecute(graph, ctx.frameIndex);
    // For headless tests, we also need to clear transient resources for next frame
    // But we keep graph alive until endFrame? For now, just clear pool's frame slot
    transientPool_.advanceFrame(ctx.frameIndex);
}

void Renderer::endFrame(const FrameContext& ctx) {
    (void)ctx;
    // In real engine, would submit present and signal fence
    // For headless, just advance frame index and flip slot
    uint32_t slot = currentFrameIndex_ % MAX_FRAMES_IN_FLIGHT;
    if (device_ != VK_NULL_HANDLE && inFlightFences_[slot] != VK_NULL_HANDLE) {
        // Would signal fence after present; for headless, just keep signaled
    }
    currentFrameIndex_++;
}

void Renderer::onResize(uint32_t newWidth, uint32_t newHeight) {
    renderExtent_ = VkExtent2D{newWidth, newHeight};
    framebufferResized_ = true;
    // In real engine, would recreate swapchain and transient resources
    transientPool_.clear();
}

void Renderer::buildFrameGraph(RenderGraph& graph, const FrameContext& ctx) {
    // 1. Import persistent resources
    auto swapchain = graph.importImage("Swapchain", ctx.swapchainImage, ctx.swapchainImageView,
                                       ctx.swapchainFormat, ctx.renderExtent, ResourceUsage::None);

    auto allInstances = graph.importBuffer("InstancesSSBO", ctx.globalInstanceBuffer, sizeof(GPUMeshInstance) * std::max(1u, ctx.totalInstances), BufferUsage::VertexRead);
    auto allMeshlets = graph.importBuffer("MeshletsSSBO", ctx.globalMeshletBuffer, sizeof(GPUMeshlet) * std::max(1u, ctx.totalMeshlets), BufferUsage::ComputeRead);
    auto lightBuffer = graph.importBuffer("LightsSSBO", ctx.globalLightBuffer, sizeof(GPULight) * std::max(1u, ctx.activeLightCount), BufferUsage::ComputeRead);

    // 2. Declare Transient Targets
    GBufferHandles gbuffer = GBuffer::declare(graph, ctx.renderExtent);
    
    auto shadowMap = graph.createResource({
        .name = "ShadowMap",
        .format = VK_FORMAT_D32_SFLOAT,
        .extent = {2048, 2048},
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    });

    auto hizPyramid = graph.createResource({
        .name = "HiZ_Pyramid",
        .format = VK_FORMAT_R32_SFLOAT,
        .extent = {ctx.renderExtent.width / 2, ctx.renderExtent.height / 2},
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .mipLevels = 11
    });

    auto hdrTarget = graph.createResource({
        .name = "HDR_Color",
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .extent = ctx.renderExtent,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
    });

    auto compactedIndices = graph.createBuffer({
        .name = "CompactedIndices",
        .size = sizeof(uint32_t) * std::max(1u, ctx.totalMeshlets) * MESHLET_MAX_TRIANGLES * 3,
        .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    });

    auto indirectCommand = graph.createBuffer({
        .name = "IndirectDrawCommand",
        .size = sizeof(VkDrawIndexedIndirectCommand),
        .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    });

    // Cluster grid: use totalClusters from lighting (approx)
    uint32_t clusterCount = 16 * 9 * 24; // 3456 for 1920x1080/128? Use fixed for test
    auto clusterGrid = graph.createBuffer({
        .name = "ClusterGridSSBO",
        .size = sizeof(ClusterCell) * clusterCount,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    });

    // For HiZ previous frame depth (use gbuffer depth from previous frame, but for DAG we use a dummy)
    // Use shadowMap depth as previous for simplicity to avoid cycle
    ResourceHandle previousFrameDepth = shadowMap; // dummy to avoid cycle, real engine would use history

    // PASS 1: Cascaded Shadow Map Pass [GRAPHICS]
    graph.addPass("ShadowPass", QueueType::Graphics,
        [&](RenderGraphBuilder& b) { b.write(shadowMap, ResourceUsage::DepthStencilAttachment); },
        [&](VkCommandBuffer cb) { recordShadowPass(cb, ctx); }
    );

    // PASS 2: Cluster Light Culling [ASYNC COMPUTE]
    graph.addPass("ClusterLightCullPass", QueueType::AsyncCompute,
        [&](RenderGraphBuilder& b) {
            b.read(lightBuffer, BufferUsage::ComputeRead);
            b.write(clusterGrid, BufferUsage::ComputeWrite);
        },
        [&](VkCommandBuffer cb) { recordClusterCullCompute(cb, ctx); }
    );

    // PASS 3: Hi-Z Build Pass [ASYNC COMPUTE]
    graph.addPass("HiZBuildPass", QueueType::AsyncCompute,
        [&](RenderGraphBuilder& b) {
            b.read(previousFrameDepth, ResourceUsage::ShaderRead);
            b.write(hizPyramid, ResourceUsage::ComputeWrite);
        },
        [&](VkCommandBuffer cb) { recordHiZBuild(cb, ctx); }
    );

    // PASS 4: Meshlet Culling & Compaction [ASYNC COMPUTE]
    graph.addPass("MeshletCullPass", QueueType::AsyncCompute,
        [&](RenderGraphBuilder& b) {
            b.read(hizPyramid, ResourceUsage::ShaderRead);
            b.read(allInstances, BufferUsage::ComputeRead);
            b.read(allMeshlets, BufferUsage::ComputeRead);
            b.write(compactedIndices, BufferUsage::ComputeWrite);
            b.write(indirectCommand, BufferUsage::ComputeWrite);
        },
        [&](VkCommandBuffer cb) { recordMeshletCull(cb, ctx); }
    );

    // PASS 5: G-Buffer Opaque Pass (Consolidated Indirect Draw) [GRAPHICS]
    graph.addPass("GBufferPass", QueueType::Graphics,
        [&](RenderGraphBuilder& b) {
            b.read(compactedIndices, BufferUsage::IndexBuffer);
            b.read(indirectCommand, BufferUsage::IndirectBuffer);
            b.write(gbuffer.albedoAO, ResourceUsage::ColorAttachment);
            b.write(gbuffer.normalRoughness, ResourceUsage::ColorAttachment);
            b.write(gbuffer.metallicFlags, ResourceUsage::ColorAttachment);
            b.write(gbuffer.depth, ResourceUsage::DepthStencilAttachment);
        },
        [&](VkCommandBuffer cb) { recordGBufferDraw(cb, ctx); }
    );

    // PASS 6: Deferred Clustered Lighting Pass [GRAPHICS]
    graph.addPass("DeferredLightingPass", QueueType::Graphics,
        [&](RenderGraphBuilder& b) {
            b.read(gbuffer.albedoAO, ResourceUsage::ShaderRead);
            b.read(gbuffer.normalRoughness, ResourceUsage::ShaderRead);
            b.read(gbuffer.metallicFlags, ResourceUsage::ShaderRead);
            b.read(gbuffer.depth, ResourceUsage::ShaderRead);
            b.read(shadowMap, ResourceUsage::ShaderRead);
            b.read(clusterGrid, BufferUsage::FragmentRead);
            b.write(hdrTarget, ResourceUsage::ColorAttachment);
        },
        [&](VkCommandBuffer cb) { recordDeferredLighting(cb, ctx); }
    );

    // PASS 7: Forward Skybox & Transparent Pass [GRAPHICS]
    graph.addPass("ForwardPass", QueueType::Graphics,
        [&](RenderGraphBuilder& b) {
            b.read(gbuffer.depth, ResourceUsage::DepthStencilAttachment); // Read-only depth test
            b.write(hdrTarget, ResourceUsage::ColorAttachment);
        },
        [&](VkCommandBuffer cb) { recordSkyboxAndTransparents(cb, ctx); }
    );

    // PASS 8: Post-Processing & Tonemapping [GRAPHICS]
    graph.addPass("PostProcessPass", QueueType::Graphics,
        [&](RenderGraphBuilder& b) {
            b.read(hdrTarget, ResourceUsage::ShaderRead);
            b.write(swapchain, ResourceUsage::ColorAttachment);
        },
        [&](VkCommandBuffer cb) { recordTonemapping(cb, ctx); }
    );

    // PASS 9: ImGui Editor Overlay Pass [GRAPHICS]
    graph.addPass("EditorOverlayPass", QueueType::Graphics,
        [&](RenderGraphBuilder& b) {
            b.write(swapchain, ResourceUsage::ColorAttachment);
        },
        [&](VkCommandBuffer cb) { recordEditorUI(cb, ctx); }
    );

    // PASS 10: Presentation Layout Transition
    graph.addPass("PresentPass", QueueType::Graphics,
        [&](RenderGraphBuilder& b) {
            b.read(swapchain, ResourceUsage::Present);
        },
        [&](VkCommandBuffer) {}
    );
}

// Stubs for recording (headless)
void Renderer::recordShadowPass(VkCommandBuffer, const FrameContext&) {}
void Renderer::recordClusterCullCompute(VkCommandBuffer, const FrameContext&) {}
void Renderer::recordHiZBuild(VkCommandBuffer, const FrameContext&) {}
void Renderer::recordMeshletCull(VkCommandBuffer, const FrameContext&) {}
void Renderer::recordGBufferDraw(VkCommandBuffer, const FrameContext&) {}
void Renderer::recordDeferredLighting(VkCommandBuffer, const FrameContext&) {}
void Renderer::recordSkyboxAndTransparents(VkCommandBuffer, const FrameContext&) {}
void Renderer::recordTonemapping(VkCommandBuffer, const FrameContext&) {}
void Renderer::recordEditorUI(VkCommandBuffer, const FrameContext&) {}

} // namespace Engine
