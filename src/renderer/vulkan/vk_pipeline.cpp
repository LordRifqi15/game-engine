#include "renderer/vulkan/vk_pipeline.h"

#include "core/instance_data.h"
#include "core/mesh.h"
#include "renderer/vulkan/vk_device.h"
#include "renderer/vulkan/vk_swapchain.h"

#include <unistd.h>
#include <limits.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace engine {

namespace {

void fatal(const char* msg) {
    std::fprintf(stderr, "Fatal: %s\n", msg);
    std::exit(EXIT_FAILURE);
}

std::string executableDir() {
    char buf[PATH_MAX];
    ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return std::string(".");
    buf[len] = '\0';
    std::string p(buf);
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

std::vector<char> readFileBytes(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "failed to open shader: %s", path);
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
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS) {
        fatal("failed to create shader module");
    }
    return module;
}

} // namespace

VulkanPipeline::VulkanPipeline(VulkanDevice& device, const VulkanSwapchain& swapchain,
                               VkDescriptorSetLayout cameraSetLayout,
                               VkDescriptorSetLayout materialSetLayout,
                               VkDescriptorSetLayout shadowSamplerSetLayout)
    : device_(device), swapchain_(swapchain) {
    VkDevice dev = device_.handle();

    const std::string shaderDir = executableDir() + "/shaders/";
    auto vertCode = readFileBytes((shaderDir + "basic.vert.spv").c_str());
    auto fragCode = readFileBytes((shaderDir + "basic.frag.spv").c_str());
    // Fixed-function state. Layout matches core/mesh.h Vertex:
    // location 0 = vec3 position, location 1 = vec3 normal.
    // Binding 0: per-vertex mesh data (divisor 0). Binding 1: per-instance
    // InstanceData (divisor 1 — advances once per instance).
    static VkVertexInputBindingDescription bindings[2]{};
    bindings[0] = {0, sizeof(engine::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    bindings[1] = {1, sizeof(engine::InstanceData), VK_VERTEX_INPUT_RATE_INSTANCE};

    // loc 0: position, loc 1: normal (binding 0)
    // loc 2-5: instance model columns, loc 6: instance color (binding 1)
    // Vertex: loc 0 = position, loc 1 = normal, loc 2 = uv.
    // Instance: model columns occupy locs 3-6, color loc 7, params loc 8.
    static VkVertexInputAttributeDescription attributes[9]{};
    attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(engine::Vertex, position)};
    attributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(engine::Vertex, normal)};
    attributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(engine::Vertex, uv)};
    for (int i = 0; i < 4; ++i) {
        attributes[3 + i] = {static_cast<uint32_t>(3 + i), 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                             offsetof(engine::InstanceData, model) + i * sizeof(glm::vec4)};
    }
    attributes[7] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(engine::InstanceData, color)};
    attributes[8] = {8, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                     offsetof(engine::InstanceData, params)}; // metallic/roughness

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = 9;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkShaderModule vert = createShaderModule(dev, vertCode);
    VkShaderModule frag = createShaderModule(dev, fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};

    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";

    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;


    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE; // Projection flips Y, so screen-winding inverts.

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.minDepthBounds = 0.0f;
    depthStencil.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    // Dynamic viewport/scissor: set at record time with actual extent.
    VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamics;

    std::array<VkDescriptorSetLayout, 3> setLayouts = {
        cameraSetLayout, materialSetLayout, shadowSamplerSetLayout};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    layoutInfo.pushConstantRangeCount = 0;
    layoutInfo.pPushConstantRanges = nullptr;
    if (vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &layout_) != VK_SUCCESS) {
        fatal("failed to create pipeline layout");
    }

    VkGraphicsPipelineCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    createInfo.stageCount = 2;
    createInfo.pStages = stages;
    createInfo.pVertexInputState = &vertexInput;
    createInfo.pInputAssemblyState = &inputAssembly;
    createInfo.pViewportState = &viewportState;
    createInfo.pRasterizationState = &rasterizer;
    createInfo.pMultisampleState = &multisampling;
    createInfo.pDepthStencilState = &depthStencil;
    createInfo.pColorBlendState = &colorBlend;
    createInfo.pDynamicState = &dynamicState;
    createInfo.layout = layout_;
    createInfo.renderPass = swapchain_.renderPass();
    createInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &createInfo, nullptr, &pipeline_) != VK_SUCCESS) {
        fatal("failed to create graphics pipeline");
    }

    vkDestroyShaderModule(dev, vert, nullptr);
    vkDestroyShaderModule(dev, frag, nullptr);
}

VulkanPipeline::~VulkanPipeline() {
    VkDevice dev = device_.handle();
    if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(dev, pipeline_, nullptr);
    if (layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, layout_, nullptr);
}

} // namespace engine
