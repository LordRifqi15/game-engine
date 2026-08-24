#include "renderer/vulkan/vk_renderer.h"

#include "platform/window.h"
#include "renderer/vulkan/vulkan_instance.h"
#include "renderer/vulkan/vk_command_buffer.h"
#include "renderer/vulkan/vk_device.h"
#include "renderer/vulkan/vk_pipeline.h"
#include "renderer/vulkan/vk_swapchain.h"

#include <cstdio>
#include <cstdlib>

namespace engine {

VkRenderer::VkRenderer(Window& window, VulkanInstance& vk)
    : window_(window), vk_(vk) {
    device_ = new VulkanDevice(vk_);
    swapchain_ = new VulkanSwapchain(window_, vk_, *device_);
    pipeline_ = new VulkanPipeline(*device_, *swapchain_,
                                   device_->cameraDescriptorLayout(),
                                   device_->materialDescriptorLayout(),
                                   device_->shadowSamplerLayout(),
                                   device_->set3Layout());
    textureCache_ = new TextureCache(*device_);
    commandBuffers_ =
        new VulkanCommandBuffer(*device_, *swapchain_, *pipeline_, *textureCache_);

    glfwSetFramebufferSizeCallback(window_.handle(), [](GLFWwindow*, int, int) {
        // Resize handled via swapchain out-of-date detection in drawFrame.
    });
}

VkRenderer::~VkRenderer() {
    if (device_) {
        device_->waitIdle();
    }
    delete commandBuffers_;
    delete pipeline_;
    delete textureCache_;
    delete swapchain_;
    delete device_;
}

void VkRenderer::beginFrame(const Camera& camera, const DirectionalLight& light) {
    // Aspect tracks current swapchain extent.
    Camera cam = camera;
    if (swapchain_->height() > 0) {
        cam.aspect = static_cast<float>(swapchain_->width()) / static_cast<float>(swapchain_->height());
    }
    viewProjection_ = cam.viewProjection();
    light_ = light;
    cameraPos_ = cam.position;
}

void VkRenderer::drawMeshInstanced(const Mesh& mesh, const std::vector<InstanceData>& instances,
                                   const Texture* texture) {
    pendingBatches_.push_back({&mesh, texture, &instances});
}

void VkRenderer::drawFrame() {
    // Buffers retired >= kMaxFramesInFlight frames ago are guaranteed unused.
    device_->waitForFence(currentFrame_);
    device_->flushRetiredScratch(currentFrame_);

    uint32_t imageIndex = 0;
    static int framesRecorded = 0, framesSkipped = 0;
    if (!swapchain_->acquireOrRecreate(currentFrame_, imageIndex)) {
        ++framesSkipped;
        std::FILE* dbg = std::fopen("/tmp/frame_probe.txt", "w");
        if (dbg) { fprintf(dbg, "recorded=%d skipped=%d", framesRecorded, framesSkipped); fclose(dbg); }
        pendingBatches_.clear();
        return; // recreated, skip this frame
    }
    ++framesRecorded;
    {
        std::FILE* dbg = std::fopen("/tmp/frame_probe.txt", "w");
        if (dbg) { fprintf(dbg, "recorded=%d skipped=%d", framesRecorded, framesSkipped); fclose(dbg); }
    }

    device_->resetFence(currentFrame_);

    commandBuffers_->recordFrame(currentFrame_, imageIndex, pendingBatches_,
                           viewProjection_, light_, cameraPos_);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphore imageAvailable = swapchain_->acquireSemaphoreForFrame(currentFrame_);
    VkSemaphore renderFinished = swapchain_->renderFinishedSemaphore(imageIndex);
    VkCommandBuffer cmd = commandBuffers_->handle(currentFrame_);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &imageAvailable;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderFinished;

    if (vkQueueSubmit(device_->graphicsQueue(), 1, &submitInfo, device_->fence(currentFrame_)) != VK_SUCCESS) {
        std::fputs("Fatal: failed to submit draw command buffer\n", stderr);
        std::exit(EXIT_FAILURE);
    }

    swapchain_->present(imageIndex, currentFrame_);

    device_->retireScratchBuffers(currentFrame_);

    pendingBatches_.clear();

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}

} // namespace engine
