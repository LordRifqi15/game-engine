#include "renderer/Renderer.hpp"
#include "renderer/deferred/GBuffer.hpp"
#include "renderer/graph/RenderGraphValidator.hpp"
#include "renderer/SceneRenderer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"

namespace Engine {

void Renderer::init(VkDevice device, VkPhysicalDevice physicalDevice,
                    const QueueFamilyIndices& queueIndices) {
    RuntimeRendererConfig config;
    config.device = device;
    config.physicalDevice = physicalDevice;
    config.queueIndices = queueIndices;
    init(config);
}

void Renderer::init(const RuntimeRendererConfig& config) {
    if (device_ != VK_NULL_HANDLE) shutdown();

    device_ = config.device;
    instance_ = config.instance;
    physicalDevice_ = config.physicalDevice;
    graphicsQueue_ = config.graphicsQueue;
    graphicsFamily_ = config.queueIndices.graphicsFamily;
    transientPool_.~TransientResourcePool();
    new (&transientPool_) TransientResourcePool(device_, physicalDevice_);
    scheduler_.~FrameScheduler();
    new (&scheduler_) FrameScheduler(device_, config.queueIndices,
                                     config.graphicsQueue, config.computeQueue,
                                     config.transferQueue);
    updateSwapchain(config.swapchain);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (device_ == VK_NULL_HANDLE) {
            inFlightFences_[i] = reinterpret_cast<VkFence>(0x6000 + i);
            imageAvailable_[i] = VK_NULL_HANDLE;
            continue;
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailable_[i]);
    }
    currentFrameIndex_ = 0;
    framebufferResized_ = false;
}

void Renderer::updateSwapchain(const RuntimeSwapchainState& swapchain) {
    if (device_ != VK_NULL_HANDLE) {
        for (auto semaphore : renderFinishedByImage_) {
            if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore, nullptr);
        }
    }
    renderFinishedByImage_.clear();
    swapchain_ = swapchain;
    renderExtent_ = swapchain_.extent.width != 0 && swapchain_.extent.height != 0
        ? swapchain_.extent : renderExtent_;
    if (device_ != VK_NULL_HANDLE && !swapchain_.images.empty()) {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        renderFinishedByImage_.resize(swapchain_.images.size(), VK_NULL_HANDLE);
        for (auto& semaphore : renderFinishedByImage_) {
            vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &semaphore);
        }
    }
}
void Renderer::shutdown() {
    shutdownEditorOverlay();
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            if (inFlightFences_[i]) vkDestroyFence(device_, inFlightFences_[i], nullptr);
            if (imageAvailable_[i]) vkDestroySemaphore(device_, imageAvailable_[i], nullptr);
        }
        for (auto semaphore : renderFinishedByImage_) {
            if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore, nullptr);
        }
    }
    for (auto& fence : inFlightFences_) fence = VK_NULL_HANDLE;
    for (auto& semaphore : imageAvailable_) semaphore = VK_NULL_HANDLE;
    renderFinishedByImage_.clear();
    scheduler_.~FrameScheduler();
    new (&scheduler_) FrameScheduler(VK_NULL_HANDLE, QueueFamilyIndices{});
    transientPool_.~TransientResourcePool();
    new (&transientPool_) TransientResourcePool(VK_NULL_HANDLE, VK_NULL_HANDLE);
    device_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    graphicsQueue_ = VK_NULL_HANDLE;
    graphicsFamily_ = UINT32_MAX;
    swapchain_ = {};
    currentFrameIndex_ = 0;
}

bool Renderer::beginFrame(FrameContext& outContext, float dt) {
    const uint32_t slot = currentFrameIndex_ % MAX_FRAMES_IN_FLIGHT;
    VkFence fence = inFlightFences_[slot];
    if (device_ != VK_NULL_HANDLE && fence != VK_NULL_HANDLE) {
        if (vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;
    }

    uint32_t imageIndex = slot;
    if (swapchain_.handle != VK_NULL_HANDLE) {
        if (swapchain_.images.empty() || swapchain_.imageViews.size() != swapchain_.images.size()) return false;
        VkResult result = vkAcquireNextImageKHR(device_, swapchain_.handle, UINT64_MAX,
                                                imageAvailable_[slot], VK_NULL_HANDLE, &imageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(device_);
            framebufferResized_ = true;
            transientPool_.clear();
            return false;
        }
        if (result != VK_SUCCESS) return false;
        framebufferResized_ = false;
        if (imageIndex >= swapchain_.images.size()) return false;
    } else if (framebufferResized_) {
        framebufferResized_ = false;
        return false;
    }

    if (renderExtent_.width == 0 || renderExtent_.height == 0) {
        framebufferResized_ = true;
        transientPool_.clear();
        return false;
    }
    if (device_ != VK_NULL_HANDLE && fence != VK_NULL_HANDLE) {
        if (vkResetFences(device_, 1, &fence) != VK_SUCCESS) return false;
        scheduler_.resetFrame(slot);
    }
    transientPool_.advanceFrame(currentFrameIndex_);

    outContext = {};
    outContext.frameIndex = currentFrameIndex_;
    outContext.frameSlot = slot;
    outContext.dt = dt;
    outContext.renderExtent = renderExtent_;
    outContext.swapchainImageIndex = imageIndex;
    outContext.swapchainFormat = swapchain_.format != VK_FORMAT_UNDEFINED
        ? swapchain_.format : VK_FORMAT_B8G8R8A8_UNORM;
    if (swapchain_.handle != VK_NULL_HANDLE) {
        outContext.swapchainImage = swapchain_.images[imageIndex];
        outContext.swapchainImageView = swapchain_.imageViews[imageIndex];
    } else {
        outContext.swapchainImage = reinterpret_cast<VkImage>(0x7000 + slot);
        outContext.swapchainImageView = reinterpret_cast<VkImageView>(0x8000 + slot);
        outContext.globalVertexBuffer = reinterpret_cast<VkBuffer>(0x9000);
        outContext.globalIndexBuffer = reinterpret_cast<VkBuffer>(0x9001);
        outContext.globalInstanceBuffer = reinterpret_cast<VkBuffer>(0x9002);
        outContext.globalMeshletBuffer = reinterpret_cast<VkBuffer>(0x9003);
        outContext.globalMaterialBuffer = reinterpret_cast<VkBuffer>(0x9004);
        outContext.globalLightBuffer = reinterpret_cast<VkBuffer>(0x9005);
        outContext.totalInstances = 1000;
        outContext.totalMeshlets = 500;
        outContext.activeLightCount = 16;
    }

    glm::vec3 camPos{5, 5, 5};
    glm::mat4 view = glm::lookAt(camPos, glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    float aspect = renderExtent_.height
        ? float(renderExtent_.width) / float(renderExtent_.height) : 1.0f;
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(60.0f), aspect, 0.1f, 100.0f);
    proj[1][1] *= -1;
    outContext.camera.viewMatrix = view;
    outContext.camera.projMatrix = proj;
    outContext.camera.invViewProj = glm::inverse(proj * view);
    outContext.camera.worldPosition = camPos;
    outContext.camera.zNear = 0.1f;
    outContext.camera.zFar = 100.0f;
    outContext.camera.fov = glm::radians(60.0f);
    outContext.camera.aspectRatio = aspect;
    return true;
}

bool Renderer::beginFrame(FrameContext& outContext, float dt, entt::registry& registry) {
    (void)registry;
    return beginFrame(outContext, dt);
}

void Renderer::renderFrame(FrameContext& ctx) {
    RenderGraph graph;
    buildFrameGraph(graph, ctx);
    std::string err;
    if (!RenderGraphValidator::validate(graph, err)) {
        std::fprintf(stderr, "[rendergraph] validator: %s\n", err.c_str());
        return;
    }
    if (!graph.compile(ctx.frameIndex, transientPool_)) {
        std::fprintf(stderr, "[rendergraph] compile failed frame %llu passes %zu:",
                     (unsigned long long)ctx.frameIndex, graph.passCount());
        for (auto& p : graph.passes()) std::fprintf(stderr, " %s", p.name.c_str());
        std::fputc('\n', stderr);
        return;
    }
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    if (ctx.swapchainImageIndex < renderFinishedByImage_.size()) {
        renderFinished = renderFinishedByImage_[ctx.swapchainImageIndex];
    }
    scheduler_.scheduleAndExecute(graph, ctx.frameIndex, ctx.frameSlot,
                                   imageAvailable_[ctx.frameSlot],
                                   renderFinished,
                                   inFlightFences_[ctx.frameSlot]);
}

void Renderer::renderFrame(FrameContext& ctx, const GPUScene* scene) {
    activeScene_ = scene;
    if (sceneRenderer_ && scene) {
        sceneRenderer_->upload(*scene, ctx, ctx.frameSlot);
    }
    renderFrame(ctx);
    activeScene_ = nullptr;
}

void Renderer::endFrame(const FrameContext& ctx) {
    if (swapchain_.handle != VK_NULL_HANDLE && swapchain_.presentQueue != VK_NULL_HANDLE &&
        ctx.swapchainImageIndex < renderFinishedByImage_.size()) {
        VkSemaphore waitSemaphore = renderFinishedByImage_[ctx.swapchainImageIndex];
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &waitSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_.handle;
        presentInfo.pImageIndices = &ctx.swapchainImageIndex;
        VkResult result = vkQueuePresentKHR(swapchain_.presentQueue, &presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            framebufferResized_ = true;
            transientPool_.clear();
        }
    }
    currentFrameIndex_++;
}

void Renderer::onResize(uint32_t newWidth, uint32_t newHeight) {
    renderExtent_ = VkExtent2D{newWidth, newHeight};
    framebufferResized_ = true;
    transientPool_.clear();
}

void Renderer::buildFrameGraph(RenderGraph& graph, const FrameContext& ctx) {
    // Task 053: authoritative scene path builds the real workload graph.
    if (sceneRenderer_ && activeScene_) {
        sceneRenderer_->buildPasses(graph, ctx);
        return;
    }
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

    VkExtent2D hizExtent{std::max(1u, ctx.renderExtent.width / 2),
                         std::max(1u, ctx.renderExtent.height / 2)};
    uint32_t hizMips = 1;
    for (uint32_t d = std::max(hizExtent.width, hizExtent.height); d > 1; d >>= 1) ++hizMips;
    auto hizPyramid = graph.createResource({
        .name = "HiZ_Pyramid",
        .format = VK_FORMAT_R32_SFLOAT,
        .extent = hizExtent,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .mipLevels = hizMips
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

// Pass recording. Full geometry/lighting pipelines land per-pass as they acquire
// live pipelines; tonemapping already clears the swapchain target so the live
// WSI path presents a defined image.
void Renderer::recordShadowPass(VkCommandBuffer, const FrameContext&) {}
void Renderer::recordClusterCullCompute(VkCommandBuffer, const FrameContext&) {}
void Renderer::recordHiZBuild(VkCommandBuffer, const FrameContext&) {}
void Renderer::recordMeshletCull(VkCommandBuffer, const FrameContext&) {}
void Renderer::recordGBufferDraw(VkCommandBuffer, const FrameContext&) {}
void Renderer::recordDeferredLighting(VkCommandBuffer, const FrameContext&) {}
void Renderer::recordSkyboxAndTransparents(VkCommandBuffer, const FrameContext&) {}
void Renderer::recordTonemapping(VkCommandBuffer cb, const FrameContext& ctx) {
    if (cb == VK_NULL_HANDLE || swapchain_.handle == VK_NULL_HANDLE) return;
    if (ctx.swapchainImageView == VK_NULL_HANDLE) return;
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = ctx.swapchainImageView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color.float32[0] = 0.05f;
    color.clearValue.color.float32[1] = 0.05f;
    color.clearValue.color.float32[2] = 0.08f;
    color.clearValue.color.float32[3] = 1.0f;
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea.offset = {0, 0};
    info.renderArea.extent = ctx.renderExtent;
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    vkCmdBeginRendering(cb, &info);
    vkCmdEndRendering(cb);
}
void Renderer::recordEditorUI(VkCommandBuffer cb, const FrameContext& ctx) {
    if (cb == VK_NULL_HANDLE || !uiReady_ || swapchain_.handle == VK_NULL_HANDLE) return;
    if (ctx.swapchainImageView == VK_NULL_HANDLE) return;
    ImGui::SetCurrentContext(uiContext_);
    ImDrawData* draw = ImGui::GetDrawData();
    if (!draw || draw->TotalVtxCount == 0) return;
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = ctx.swapchainImageView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea.offset = {0, 0};
    info.renderArea.extent = ctx.renderExtent;
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    vkCmdBeginRendering(cb, &info);
    ImGui_ImplVulkan_RenderDrawData(draw, cb);
    vkCmdEndRendering(cb);
}

void Renderer::initEditorOverlay(GLFWwindow* window, VkFormat colorFormat) {
    if (uiReady_ || window == nullptr) return;
#ifndef IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING
    return;
#else
    if (device_ == VK_NULL_HANDLE || instance_ == VK_NULL_HANDLE ||
        physicalDevice_ == VK_NULL_HANDLE || graphicsQueue_ == VK_NULL_HANDLE ||
        graphicsFamily_ == UINT32_MAX) {
        return;
    }
#endif
    uiContext_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(uiContext_);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    VkDescriptorPoolSize pools[4]{};
    pools[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
    pools[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16};
    pools[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16};
    pools[3] = {VK_DESCRIPTOR_TYPE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 64;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes = pools;
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &uiPool_) != VK_SUCCESS) {
        ImGui::DestroyContext();
        uiContext_ = nullptr;
        return;
    }

    ImGui_ImplGlfw_InitForVulkan(window, true);
    ImGui_ImplVulkan_InitInfo init{};
    init.Instance = instance_;
    init.PhysicalDevice = physicalDevice_;
    init.Device = device_;
    init.QueueFamily = graphicsFamily_;
    init.Queue = graphicsQueue_;
    init.DescriptorPool = uiPool_;
    init.MinImageCount = 2;
    init.ImageCount = std::max(2u, static_cast<uint32_t>(swapchain_.images.size()));
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.UseDynamicRendering = true;
    init.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;
    if (!ImGui_ImplVulkan_Init(&init)) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        uiContext_ = nullptr;
        vkDestroyDescriptorPool(device_, uiPool_, nullptr);
        uiPool_ = VK_NULL_HANDLE;
        return;
    }
    uiReady_ = true;
}

void Renderer::shutdownEditorOverlay() {
    if (!uiReady_) return;
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
    ImGui::SetCurrentContext(uiContext_);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    uiContext_ = nullptr;
    if (device_ != VK_NULL_HANDLE && uiPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, uiPool_, nullptr);
    }
    uiPool_ = VK_NULL_HANDLE;
    uiReady_ = false;
}

void Renderer::editorBegin() {
    if (!uiReady_) return;
    ImGui::SetCurrentContext(uiContext_);
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void Renderer::editorEnd() {
    if (!uiReady_) return;
    ImGui::SetCurrentContext(uiContext_);
    ImGui::Render();
}

} // namespace Engine
