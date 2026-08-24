#include "renderer/vulkan/shadow_pass.h"

#include "renderer/vulkan/vk_device.h"
#include "renderer/vulkan/vk_swapchain.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

namespace engine {

namespace {

void fatal(const char* msg) {
    std::fprintf(stderr, "Fatal: %s\n", msg);
    std::exit(EXIT_FAILURE);
}

std::string exeDir() {
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return ".";
    std::string p(buf, static_cast<size_t>(len));
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

std::vector<char> readFileBytes(const std::string& path) {
    const std::string exeDirVal = exeDir();
    std::string full = path;
    for (const std::string& base : {exeDirVal + "/", exeDirVal + "/../"}) {
        FILE* probe = std::fopen((base + path).c_str(), "rb");
        if (probe) { std::fclose(probe); full = base + path; break; }
    }
    FILE* f = std::fopen(full.c_str(), "rb");
    if (!f) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "failed to open shader: %s", full.c_str());
        fatal(buf);
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::rewind(f);
    std::vector<char> bytes(static_cast<size_t>(size));
    if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
        std::fclose(f);
        fatal("short read on shader file");
    }
    std::fclose(f);
    return bytes;
}

VkShaderModule createShaderModule(VkDevice device, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &m) != VK_SUCCESS)
        fatal("failed to create shadow shader module");
    return m;
}

} // namespace

VulkanShadowPass::VulkanShadowPass(VulkanDevice& device,
                                   const VulkanSwapchain& swapchain)
    : device_(device), swapchain_(swapchain) {
    createResources();
    createRenderPass();
    createPipeline();
}

VulkanShadowPass::~VulkanShadowPass() {
    VkDevice dev = device_.handle();
    if (pipeline_) vkDestroyPipeline(dev, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
    if (sampler_) vkDestroySampler(dev, sampler_, nullptr);
    for (uint32_t c = 0; c < kCascadeCount; ++c) {
        if (framebuffers_[c]) vkDestroyFramebuffer(dev, framebuffers_[c], nullptr);
        if (views_[c]) vkDestroyImageView(dev, views_[c], nullptr);
        if (images_[c]) vkDestroyImage(dev, images_[c], nullptr);
        if (memories_[c]) vkFreeMemory(dev, memories_[c], nullptr);
    }
    if (renderPass_) vkDestroyRenderPass(dev, renderPass_, nullptr);
}

void VulkanShadowPass::createResources() {
    VkDevice dev = device_.handle();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;
    imageInfo.extent = {kSize, kSize, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage =
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    for (uint32_t c = 0; c < kCascadeCount; ++c) {
        if (vkCreateImage(dev, &imageInfo, nullptr, &images_[c]) != VK_SUCCESS)
            fatal("failed to create shadow image");

        VkMemoryRequirements reqs{};
        vkGetImageMemoryRequirements(dev, images_[c], &reqs);
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(device_.physical(), &mp);
        uint32_t memType = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((reqs.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memType = i;
                break;
            }
        }
        if (memType == UINT32_MAX) fatal("no device-local memory for shadow map");

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = reqs.size;
        allocInfo.memoryTypeIndex = memType;
        if (vkAllocateMemory(dev, &allocInfo, nullptr, &memories_[c]) != VK_SUCCESS)
            fatal("failed to allocate shadow memory");
        if (vkBindImageMemory(dev, images_[c], memories_[c], 0) != VK_SUCCESS)
            fatal("failed to bind shadow image");

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images_[c];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(dev, &viewInfo, nullptr, &views_[c]) != VK_SUCCESS)
            fatal("failed to create shadow view");
    }

    // One shared sampler — manual PCF taps in the fragment shader.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(dev, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS)
        fatal("failed to create shadow sampler");
}

void VulkanShadowPass::createRenderPass() {
    VkDevice dev = device_.handle();

    VkAttachmentDescription depth{};
    depth.format = VK_FORMAT_D32_SFLOAT;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthRef;

    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &depth;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    if (vkCreateRenderPass(dev, &rp, nullptr, &renderPass_) != VK_SUCCESS)
        fatal("failed to create shadow render pass");

    for (uint32_t c = 0; c < kCascadeCount; ++c) {
        VkFramebufferCreateInfo fb{};
        fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass = renderPass_;
        fb.attachmentCount = 1;
        fb.pAttachments = &views_[c];
        fb.width = kSize;
        fb.height = kSize;
        fb.layers = 1;
        if (vkCreateFramebuffer(dev, &fb, nullptr, &framebuffers_[c]) != VK_SUCCESS)
            fatal("failed to create shadow framebuffer");
    }
}

void VulkanShadowPass::begin(VkCommandBuffer cmd, uint32_t cascade) {
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = renderPass_;
    passInfo.framebuffer = framebuffers_[cascade];
    passInfo.renderArea.offset = {0, 0};
    passInfo.renderArea.extent = {kSize, kSize};
    passInfo.clearValueCount = 1;
    passInfo.pClearValues = &clear;

    vkCmdBeginRenderPass(cmd, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
}

void VulkanShadowPass::drawBatch(VkCommandBuffer cmd, const Mesh& mesh,
                                 uint32_t instanceCount, const glm::mat4& lightVP) {
    if (instanceCount == 0 || mesh.indices.empty() || mesh.vertices.empty()) return;

    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(glm::mat4), &lightVP);

    VkBuffer vertexBuffer = device_.scratchVertexBuffer(mesh.vertices);
    VkBuffer buffers[] = {vertexBuffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);

    VkBuffer indexBuffer = device_.scratchIndexBuffer(mesh.indices);
    vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(mesh.indices.size()),
                     instanceCount, 0, 0, 0);
}

void VulkanShadowPass::end(VkCommandBuffer cmd) {
    vkCmdEndRenderPass(cmd);
}

void VulkanShadowPass::createPipeline() {
    VkDevice dev = device_.handle();

    auto vertCode = readFileBytes("shaders/shadow.vert.spv");
    auto fragCode = readFileBytes("shaders/shadow.frag.spv");
    VkShaderModule vert = createShaderModule(dev, vertCode);
    VkShaderModule frag = createShaderModule(dev, fragCode);

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(glm::mat4); // per-cascade lightVP
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS)
        fatal("failed to create shadow pipeline layout");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    // Same vertex layout as the main pipeline (mesh + instance model columns).
    static VkVertexInputBindingDescription bindings[2]{};
    bindings[0] = {0, sizeof(engine::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    bindings[1] = {1, sizeof(engine::InstanceData), VK_VERTEX_INPUT_RATE_INSTANCE};

    static VkVertexInputAttributeDescription attributes[5]{};
    attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(engine::Vertex, position)};
    for (int i = 0; i < 4; ++i) {
        attributes[1 + i] = {static_cast<uint32_t>(1 + i), 1,
                             VK_FORMAT_R32G32B32A32_SFLOAT,
                             offsetof(engine::InstanceData, model) + i * sizeof(glm::vec4)};
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = 5;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{0.0f, 0.0f, static_cast<float>(kSize),
                        static_cast<float>(kSize), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {kSize, kSize}};
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE; // both windings: light sees all faces
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.depthBiasConstantFactor = 1.5f;
    rasterizer.depthBiasSlopeFactor = 2.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkGraphicsPipelineCreateInfo pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeInfo.stageCount = 2;
    pipeInfo.pStages = stages;
    pipeInfo.pVertexInputState = &vertexInput;
    pipeInfo.pInputAssemblyState = &inputAssembly;
    pipeInfo.pViewportState = &viewportState;
    pipeInfo.pRasterizationState = &rasterizer;
    pipeInfo.pMultisampleState = &multisampling;
    pipeInfo.pDepthStencilState = &depthStencil;
    pipeInfo.layout = pipelineLayout_;
    pipeInfo.renderPass = renderPass_;
    pipeInfo.subpass = 0;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeInfo, nullptr,
                                  &pipeline_) != VK_SUCCESS)
        fatal("failed to create shadow pipeline");

    vkDestroyShaderModule(dev, vert, nullptr);
    vkDestroyShaderModule(dev, frag, nullptr);
}

} // namespace engine
