#include "renderer/vulkan/gpu_occlusion.h"

#include "core/instance_data.h"
#include "renderer/vulkan/vk_device.h"
#include "renderer/vulkan/vk_swapchain.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace engine {

namespace {

void fatal(const char* msg) {
    std::fprintf(stderr, "Fatal: %s\n", msg);
    std::exit(EXIT_FAILURE);
}

uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

VkImageView makeView(VkDevice dev, VkImage image, VkFormat format, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &vi, nullptr, &view) != VK_SUCCESS)
        fatal("image view create");
    return view;
}

void allocAndBind(VkDevice dev, VkPhysicalDevice phys, VkImage image,
                  VkDeviceMemory* memory) {
    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(dev, image, &reqs);
    uint32_t type = findMemoryType(phys, reqs.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) fatal("image memory type");
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = reqs.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(dev, &ai, nullptr, memory) != VK_SUCCESS)
        fatal("image memory alloc");
    vkBindImageMemory(dev, image, *memory, 0);
}

std::vector<char> readSpv(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) fatal((std::string("open shader: ") + path).c_str());
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::rewind(f);
    std::vector<char> bytes(static_cast<size_t>(size));
    if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
        std::fclose(f);
        fatal("short shader read");
    }
    std::fclose(f);
    return bytes;
}

std::string exeRelative(const char* rel) {
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    std::string exeDir = len > 0 ? std::string(buf, static_cast<size_t>(len)) : ".";
    auto slash = exeDir.find_last_of('/');
    if (slash != std::string::npos) exeDir = exeDir.substr(0, slash);
    for (const std::string& base : {exeDir + "/", exeDir + "/../"}) {
        FILE* p = std::fopen((base + rel).c_str(), "rb");
        if (p) { std::fclose(p); return base + rel; }
    }
    return std::string(rel);
}

} // namespace

VulkanGpuOcclusion::VulkanGpuOcclusion(VulkanDevice& device,
                                       const VulkanSwapchain& swapchain)
    : device_(device), swapchain_(swapchain) {
    auto [w, h] = swapchain_.extent();
    width_ = w > 0 ? w : 1920;
    height_ = h > 0 ? h : 1080;
    createDepthResources();
    createHizResources();
    createRenderPassAndFramebuffer();
    createPrepassPipeline();
}

VulkanGpuOcclusion::~VulkanGpuOcclusion() {
    VkDevice dev = device_.handle();
    if (prepassPipeline_) vkDestroyPipeline(dev, prepassPipeline_, nullptr);
    if (prepassLayout_) vkDestroyPipelineLayout(dev, prepassLayout_, nullptr);
    if (hizView_) vkDestroyImageView(dev, hizView_, nullptr);
    if (hizImage_) vkDestroyImage(dev, hizImage_, nullptr);
    if (hizMemory_) vkFreeMemory(dev, hizMemory_, nullptr);
    if (depthFramebuffer_) vkDestroyFramebuffer(dev, depthFramebuffer_, nullptr);
    if (depthRenderPass_) vkDestroyRenderPass(dev, depthRenderPass_, nullptr);
    if (depthView_) vkDestroyImageView(dev, depthView_, nullptr);
    if (depthImage_) vkDestroyImage(dev, depthImage_, nullptr);
    if (depthMemory_) vkFreeMemory(dev, depthMemory_, nullptr);
}

void VulkanGpuOcclusion::createDepthResources() {
    VkDevice dev = device_.handle();
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_D32_SFLOAT;
    ii.extent = {width_, height_, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    // Sampled so hiz.comp can texelFetch the depth directly.
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(dev, &ii, nullptr, &depthImage_) != VK_SUCCESS)
        fatal("prepass depth image");
    allocAndBind(dev, device_.physical(), depthImage_, &depthMemory_);
    depthView_ = makeView(dev, depthImage_, VK_FORMAT_D32_SFLOAT,
                          VK_IMAGE_ASPECT_DEPTH_BIT);
}

void VulkanGpuOcclusion::createHizResources() {
    VkDevice dev = device_.handle();
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R32_SFLOAT;
    ii.extent = {kHizSize, kHizSize, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(dev, &ii, nullptr, &hizImage_) != VK_SUCCESS)
        fatal("hiz image");
    allocAndBind(dev, device_.physical(), hizImage_, &hizMemory_);
    hizView_ = makeView(dev, hizImage_, VK_FORMAT_R32_SFLOAT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
}

void VulkanGpuOcclusion::createRenderPassAndFramebuffer() {
    VkDevice dev = device_.handle();

    VkAttachmentDescription depth{};
    depth.format = VK_FORMAT_D32_SFLOAT;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference ref{};
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &ref;

    // External dependency: depth writes finish at end of subpass 0; hiz.comp
    // (compute, outside the pass) reads afterwards.
    VkSubpassDependency dep{};
    dep.srcSubpass = 0;
    dep.dstSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1;
    rp.pAttachments = &depth;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    if (vkCreateRenderPass(dev, &rp, nullptr, &depthRenderPass_) != VK_SUCCESS)
        fatal("prepass render pass");

    VkFramebufferCreateInfo fb{};
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass = depthRenderPass_;
    fb.attachmentCount = 1;
    fb.pAttachments = &depthView_;
    fb.width = width_;
    fb.height = height_;
    fb.layers = 1;
    if (vkCreateFramebuffer(dev, &fb, nullptr, &depthFramebuffer_) != VK_SUCCESS)
        fatal("prepass framebuffer");
}

void VulkanGpuOcclusion::createPrepassPipeline() {
    VkDevice dev = device_.handle();

    // Reuse shadow.vert (push-constant mat4, same vertex layout as main pass).
    std::vector<char> code = readSpv(exeRelative("shaders/shadow.vert.spv"));
    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = code.size();
    mi.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule vert = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &mi, nullptr, &vert) != VK_SUCCESS)
        fatal("prepass vert module");

    std::vector<char> fcode = readSpv(exeRelative("shaders/prepass.frag.spv"));
    VkShaderModuleCreateInfo fmi{};
    fmi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    fmi.codeSize = fcode.size();
    fmi.pCode = reinterpret_cast<const uint32_t*>(fcode.data());
    VkShaderModule frag = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &fmi, nullptr, &frag) != VK_SUCCESS)
        fatal("prepass frag module");

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset = 0;
    push.size = sizeof(float) * 16;

    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &li, nullptr, &prepassLayout_) != VK_SUCCESS)
        fatal("prepass layout");

    // Same vertex layout as shadow/main: position + instance model (loc 1..4).
    static VkVertexInputBindingDescription bindings[2]{};
    bindings[0] = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    bindings[1] = {1, sizeof(InstanceData), VK_VERTEX_INPUT_RATE_INSTANCE};
    static VkVertexInputAttributeDescription attrs[7]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)};
    for (int i = 0; i < 4; ++i)
        attrs[1 + i] = {static_cast<uint32_t>(1 + i), 1,
                        VK_FORMAT_R32G32B32A32_SFLOAT,
                        offsetof(InstanceData, model) +
                            static_cast<uint32_t>(i * sizeof(glm::vec4))};
    attrs[5] = {9, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, jointIndices)};
    attrs[6] = {10, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, jointWeights)};

    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = 2;
    vin.pVertexBindingDescriptions = bindings;
    vin.vertexAttributeDescriptionCount = 7;
    vin.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vs{};
    vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vs.viewportCount = 1;
    vs.scissorCount = 1;

    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.lineWidth = 1.0f;
    rs.cullMode = VK_CULL_MODE_NONE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkGraphicsPipelineCreateInfo pipe{};
    pipe.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipe.stageCount = 2;
    pipe.pStages = stages;
    pipe.pVertexInputState = &vin;
    pipe.pInputAssemblyState = &ia;
    pipe.pViewportState = &vs;
    pipe.pRasterizationState = &rs;
    pipe.pMultisampleState = &ms;
    pipe.pDepthStencilState = &ds;
    pipe.pDynamicState = &dyn;
    pipe.layout = prepassLayout_;
    pipe.renderPass = depthRenderPass_;
    pipe.subpass = 0;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipe, nullptr,
                                  &prepassPipeline_) != VK_SUCCESS)
        fatal("prepass pipeline");

    vkDestroyShaderModule(dev, vert, nullptr);
    vkDestroyShaderModule(dev, frag, nullptr);
}

void VulkanGpuOcclusion::beginPrepass(VkCommandBuffer cmd) {
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass = depthRenderPass_;
    bi.framebuffer = depthFramebuffer_;
    bi.renderArea.extent = {width_, height_};
    bi.clearValueCount = 1;
    bi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prepassPipeline_);
    VkViewport vp{0, 0, float(width_), float(height_), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {width_, height_}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void VulkanGpuOcclusion::drawPrepass(VkCommandBuffer cmd, const Mesh& mesh,
                                     uint32_t instanceCount,
                                     const glm::mat4& viewProjection,
                                     VkBuffer instanceBuffer) {
    if (mesh.indices.empty() || mesh.vertices.empty() || instanceCount == 0)
        return;
    vkCmdPushConstants(cmd, prepassLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(float) * 16, &viewProjection);
    VkBuffer vb = device_.scratchVertexBuffer(mesh.vertices);
    VkBuffer buffers[] = {vb, instanceBuffer};
    VkDeviceSize offsets[] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
    VkBuffer ib = device_.scratchIndexBuffer(mesh.indices);
    vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(mesh.indices.size()),
                     instanceCount, 0, 0, 0);
}

void VulkanGpuOcclusion::endPrepass(VkCommandBuffer cmd) {
    vkCmdEndRenderPass(cmd);
}

} // namespace engine
