#include "renderer/vulkan/vk_device.h"

#include "renderer/vulkan/vulkan_instance.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <array>
#include <cstring>



namespace engine {

namespace {

void fatal(const char* msg) {
    std::fprintf(stderr, "Fatal: %s\n", msg);
    std::exit(EXIT_FAILURE);
}

} // namespace

VulkanDevice::VulkanDevice(VulkanInstance& vk)
    : vk_(vk) {
    uint32_t count = 0;
    VkInstance instance = vk_.handle();
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) fatal("no Vulkan-capable GPU found");
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    for (auto d : devices) {
        QueueFamilies q = findQueueFamilies(d);

        // Swapchain support check (needs surface queries).
        VkSurfaceKHR surface = vk_.surface();
        uint32_t formatCount = 0, presentCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(d, surface, &formatCount, nullptr);
        vkGetPhysicalDeviceSurfacePresentModesKHR(d, surface, &presentCount, nullptr);

        if (q.complete() && formatCount > 0 && presentCount > 0) {
            physical_ = d;
            graphicsFamily_ = static_cast<uint32_t>(q.graphics);
            presentFamily_ = static_cast<uint32_t>(q.present);
            break;
        }
    }
    if (physical_ == VK_NULL_HANDLE) fatal("no suitable GPU with graphics + present queues");

    // Logical device.
    float priority = 1.0f;
    std::set<uint32_t> uniqueFamilies{graphicsFamily_, presentFamily_};
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        info.queueFamilyIndex = family;
        info.queueCount = 1;
        info.pQueuePriorities = &priority;
        queueInfos.push_back(info);
    }

    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    createInfo.pQueueCreateInfos = queueInfos.data();
    createInfo.pEnabledFeatures = &features;

    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    createInfo.enabledExtensionCount = 1;
    createInfo.ppEnabledExtensionNames = deviceExtensions;

    // Device layers deprecated since 1.0: never enable. Instance layers cover everything.

    if (vkCreateDevice(physical_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
        fatal("failed to create logical device");
    }

    vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, presentFamily_, 0, &presentQueue_);

    createCameraDescriptorLayout();
    createMaterialDescriptorLayout();
    createSet3Layout();
    createShadowSamplerLayout();
    createCommandPool();
}

VulkanDevice::~VulkanDevice() {
    for (uint32_t i = 0; i < 2; ++i) flushRetiredScratch(i);
    if (materialDescriptorLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, materialDescriptorLayout_, nullptr);
    if (shadowSamplerLayout_ != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(device_, shadowSamplerLayout_, nullptr);
    if (cameraDescriptorLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, cameraDescriptorLayout_, nullptr);
    }
    for (size_t i = 0; i < uniformBuffers_.size(); ++i) {
        if (uniformMapped_[i]) vkUnmapMemory(device_, uniformMemories_[i]);
        vkDestroyBuffer(device_, uniformBuffers_[i], nullptr);
        vkFreeMemory(device_, uniformMemories_[i], nullptr);
    }
    for (auto buffer : scratchBuffers_) vkDestroyBuffer(device_, buffer, nullptr);
    for (auto mem : scratchMemories_) vkFreeMemory(device_, mem, nullptr);
    for (auto fence : fences_) vkDestroyFence(device_, fence, nullptr);
    if (commandPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, commandPool_, nullptr);
    if (device_ != VK_NULL_HANDLE) vkDestroyDevice(device_, nullptr);
}

VulkanDevice::QueueFamilies VulkanDevice::findQueueFamilies(VkPhysicalDevice device) const {
    QueueFamilies out;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            out.graphics = static_cast<int>(i);
        }
        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, vk_.surface(), &presentSupport);
        if (presentSupport) {
            out.present = static_cast<int>(i);
        }
        if (out.complete()) break;
    }
    return out;
}

void VulkanDevice::createCommandPool() {
    VkCommandPoolCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    createInfo.queueFamilyIndex = graphicsFamily_;

    if (vkCreateCommandPool(device_, &createInfo, nullptr, &commandPool_) != VK_SUCCESS) {
        fatal("failed to create command pool");
    }
}

VkCommandBuffer VulkanDevice::allocateCommandBuffer() {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(device_, &allocInfo, &cmd) != VK_SUCCESS) {
        fatal("failed to allocate command buffer");
    }
    return cmd;
}

void VulkanDevice::createFrameFences(uint32_t frameCount) {
    fences_.resize(frameCount);
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (auto& fence : fences_) {
        if (vkCreateFence(device_, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
            fatal("failed to create frame fence");
        }
    }
}

namespace {

uint32_t findMemoryType(VkPhysicalDevice physical, uint32_t typeFilter,
                        VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    fatal("failed to find suitable memory type");
    return 0;
}

} // namespace

void VulkanDevice::createCameraDescriptorLayout() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    // Camera UBO read by vertex (transform) AND fragment (lighting).
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &cameraDescriptorLayout_) != VK_SUCCESS) {
        fatal("failed to create camera descriptor set layout");
    }
}

void VulkanDevice::createMaterialDescriptorLayout() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &materialDescriptorLayout_) != VK_SUCCESS)
        fatal("failed to create material descriptor set layout");
}

void VulkanDevice::createSet3Layout() {
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (uint32_t b = 0; b < 4; ++b) {
        bindings[b].binding = b;
        bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[b].descriptorCount = 1;
        bindings[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &set3Layout_) != VK_SUCCESS)
        fatal("failed to create set3 layout");
}

void VulkanDevice::createShadowSamplerLayout() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr,
                                    &shadowSamplerLayout_) != VK_SUCCESS)
        fatal("failed to create shadow sampler set layout");
}

void VulkanDevice::createUniformBuffers(uint32_t frameCount, VkDeviceSize size) {
    uniformBuffers_.resize(frameCount);
    uniformMemories_.resize(frameCount);
    uniformMapped_.resize(frameCount);

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    for (uint32_t i = 0; i < frameCount; ++i) {
        if (vkCreateBuffer(device_, &bufInfo, nullptr, &uniformBuffers_[i]) != VK_SUCCESS) {
            fatal("failed to create uniform buffer");
        }
        VkMemoryRequirements reqs{};
        vkGetBufferMemoryRequirements(device_, uniformBuffers_[i], &reqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = reqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(
            physical_, reqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(device_, &allocInfo, nullptr, &uniformMemories_[i]) != VK_SUCCESS) {
            fatal("failed to allocate uniform memory");
        }
        if (vkBindBufferMemory(device_, uniformBuffers_[i], uniformMemories_[i], 0) != VK_SUCCESS) {
            fatal("failed to bind uniform buffer");
        }
        // Persistently mapped: coherent memory keeps CPU writes visible.
        if (vkMapMemory(device_, uniformMemories_[i], 0, size, 0, &uniformMapped_[i]) != VK_SUCCESS) {
            fatal("failed to map uniform memory");
        }
    }
}

void* VulkanDevice::mapUniform(uint32_t frameIndex) {
    return uniformMapped_[frameIndex];
}

void VulkanDevice::unmapUniform(uint32_t frameIndex) {
    vkUnmapMemory(device_, uniformMemories_[frameIndex]);
    uniformMapped_[frameIndex] = nullptr;
}

void VulkanDevice::retireScratchBuffers(uint32_t frameIndex) {
    for (auto b : scratchBuffers_) retiredBuffers_[frameIndex].push_back(b);
    for (auto m : scratchMemories_) retiredMemories_[frameIndex].push_back(m);
    scratchBuffers_.clear();
    scratchMemories_.clear();
}

void VulkanDevice::flushRetiredScratch(uint32_t frameIndex) {
    for (auto b : retiredBuffers_[frameIndex]) vkDestroyBuffer(device_, b, nullptr);
    for (auto m : retiredMemories_[frameIndex]) vkFreeMemory(device_, m, nullptr);
    retiredBuffers_[frameIndex].clear();
    retiredMemories_[frameIndex].clear();
}

// ponytail: host-visible buffer per draw call; replace with persistent GPU
// buffers + staging pool when mesh count justifies it.
VkBuffer VulkanDevice::scratchVertexBuffer(const void* data, size_t bytes) {
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = bytes;
    bufInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(device_, &bufInfo, nullptr, &buffer) != VK_SUCCESS) {
        fatal("failed to create scratch vertex buffer");
    }

    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device_, buffer, &reqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = reqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        physical_, reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device_, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        fatal("failed to allocate scratch vertex memory");
    }

    void* mapped = nullptr;
    vkMapMemory(device_, memory, 0, bytes, 0, &mapped);
    std::memcpy(mapped, data, bytes);
    vkUnmapMemory(device_, memory);

    if (vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS) {
        fatal("failed to bind scratch vertex buffer");
    }

    scratchBuffers_.push_back(buffer);
    scratchMemories_.push_back(memory);
    return buffer;
}

VkBuffer VulkanDevice::scratchIndexBuffer(const void* data, size_t bytes) {
    // Same path as vertex: usage flag differs.
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = bytes;
    bufInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(device_, &bufInfo, nullptr, &buffer) != VK_SUCCESS) {
        fatal("failed to create scratch index buffer");
    }

    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device_, buffer, &reqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = reqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        physical_, reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device_, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        fatal("failed to allocate scratch index memory");
    }

    void* mapped = nullptr;
    vkMapMemory(device_, memory, 0, bytes, 0, &mapped);
    std::memcpy(mapped, data, bytes);
    vkUnmapMemory(device_, memory);

    if (vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS) {
        fatal("failed to bind scratch index buffer");
    }

    scratchBuffers_.push_back(buffer);
    scratchMemories_.push_back(memory);
    return buffer;
}

} // namespace engine
