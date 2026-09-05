#include "renderer/vulkan/vk_renderer.h"

#include "platform/window.h"
#include "renderer/vulkan/vulkan_instance.h"
#include "renderer/vulkan/vk_command_buffer.h"
#include "renderer/vulkan/vk_device.h"
#include "renderer/vulkan/vk_pipeline.h"
#include "renderer/vulkan/vk_swapchain.h"

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"

#include <cstdio>
#include <cstdlib>

namespace engine {
static void imguiOverlayDraw(VkCommandBuffer cmd) {
    if (auto* draw = ImGui::GetDrawData()) ImGui_ImplVulkan_RenderDrawData(draw, cmd);
}



VkRenderer::VkRenderer(Window& window, VulkanInstance& vk)
    : window_(window), vk_(vk) {
    device_ = new VulkanDevice(vk_);
    swapchain_ = new VulkanSwapchain(window_, vk_, *device_);
    pipeline_ = new VulkanPipeline(*device_, *swapchain_,
                                   device_->cameraDescriptorLayout(),
                                   device_->materialDescriptorLayout(),
                                   device_->shadowSamplerLayout(),
                                   device_->set3Layout(),
                                   device_->skinningDescriptorLayout());
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
    if (editorEnabled_) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        if (imguiPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_->handle(), imguiPool_, nullptr);
            imguiPool_ = VK_NULL_HANDLE;
        }
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

void VkRenderer::drawMeshInstanced(const Mesh& mesh, const InstanceData& instance,
                                   const Texture* texture) {
    // Queue per-instance data for CPU upload to SSBO.
    allInstances_.push_back(instance);
    PendingBatch batch;
    batch.mesh = &mesh;
    batch.texture = texture;
    batch.firstInstance = nextInstanceOffset_;
    batch.instanceCount = 1; // one instance per drawMeshInstanced call
    ++nextInstanceOffset_;
    pendingBatches_.push_back(batch);
}

void VkRenderer::updateJoints(const std::vector<glm::mat4>& joints) {
    commandBuffers_->updateJoints(currentFrame_, joints);
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
    allInstances_.clear();
    nextInstanceOffset_ = 0;
        return; // recreated, skip this frame
    }
    ++framesRecorded;
    {
        std::FILE* dbg = std::fopen("/tmp/frame_probe.txt", "w");
        if (dbg) { fprintf(dbg, "recorded=%d skipped=%d", framesRecorded, framesSkipped); fclose(dbg); }
    }

    device_->resetFence(currentFrame_);

    commandBuffers_->recordFrame(currentFrame_, imageIndex, pendingBatches_,
                                 allInstances_, viewProjection_, light_,
                                 cameraPos_);

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
    allInstances_.clear();
    nextInstanceOffset_ = 0;

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}

VkDevice VkRenderer::deviceHandle() const { return device_->handle(); }
VkInstance VkRenderer::instanceHandle() const { return vk_.handle(); }
VkPhysicalDevice VkRenderer::physicalDeviceHandle() const { return device_->physical(); }
VkQueue VkRenderer::graphicsQueueHandle() const { return device_->graphicsQueue(); }
uint32_t VkRenderer::graphicsFamilyIndex() const { return device_->graphicsFamily(); }
VkQueue VkRenderer::presentQueueHandle() const { return device_->presentQueue(); }
VkSwapchainKHR VkRenderer::swapchainHandle() const { return swapchain_->handle(); }
const std::vector<VkImage>& VkRenderer::swapchainImages() const { return swapchain_->images(); }
const std::vector<VkImageView>& VkRenderer::swapchainImageViews() const { return swapchain_->views(); }
VkFormat VkRenderer::swapchainFormat() const { return swapchain_->format(); }
VkExtent2D VkRenderer::swapchainExtent() const { return swapchain_->extent(); }
void VkRenderer::recreateSwapchain() { swapchain_->recreate(); }
VulkanEnvironment& VkRenderer::environment() { return commandBuffers_->environment(); }


// ---- Editor overlay (Task 031) ----

void VkRenderer::enableEditorOverlay(Window& window) {
    if (editorEnabled_) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    VkDescriptorPoolSize pools[4]{};
    pools[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32};
    pools[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16};
    pools[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16};
    pools[3] = {VK_DESCRIPTOR_TYPE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 32;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes = pools;
    if (vkCreateDescriptorPool(device_->handle(), &poolInfo, nullptr, &imguiPool_) != VK_SUCCESS) {
        std::fprintf(stderr, "Failed to create ImGui descriptor pool\n");
        return;
    }

    ImGui_ImplGlfw_InitForVulkan(window.handle(), true);
    ImGui_ImplVulkan_InitInfo init = {};
    init.Instance = vk_.handle();
    init.PhysicalDevice = device_->physical();
    init.Device = device_->handle();
    init.QueueFamily = device_->graphicsFamily();
    init.Queue = device_->graphicsQueue();
    init.DescriptorPool = imguiPool_;
    init.RenderPass = swapchain_->renderPass();
    init.MinImageCount = 2;
    init.ImageCount = static_cast<uint32_t>(swapchain_->imageCount());
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.CheckVkResultFn = nullptr;
    ImGui_ImplVulkan_Init(&init);

    commandBuffers_->setOverlay(imguiOverlayDraw);
    editorEnabled_ = true;
}

void VkRenderer::editorBeginFrame() {
    if (!editorEnabled_) return;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void VkRenderer::editorEndFrame() {
    if (!editorEnabled_) return;
    ImGui::Render();
}

} // namespace engine
