#include "renderer/vulkan/environment.h"

#include "renderer/vulkan/vk_device.h"
#include "renderer/vulkan/vk_swapchain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace engine {

namespace {

void fatal(const char* msg) {
    std::fprintf(stderr, "Fatal: %s\n", msg);
    std::exit(EXIT_FAILURE);
}

constexpr uint32_t kEnvSize = 128;      // source env face size (LDR)
constexpr uint32_t kIrrSize = 32;       // irradiance face size
constexpr uint32_t kPrefSize = 128;     // prefiltered base face size
constexpr uint32_t kBrdfSize = 256;
constexpr float kSunIntensity = 4.0f;
constexpr float PI_f = 3.14159265358979f;

// GL-convention cubemap direction for texel (x, y) on `face`.


// Sample the CPU-side env faces by direction (major-axis lookup).
glm::vec3 sampleEnvFaces(const std::vector<std::vector<unsigned char>>& faces,
                         uint32_t size, glm::vec3 d) {
    glm::vec3 a = glm::abs(d);
    uint32_t face;
    float sc, tc;
    if (a.x >= a.y && a.x >= a.z) {
        face = d.x > 0 ? 0 : 1;
        sc = d.x > 0 ? -d.z : d.z;
        tc = -d.y;
    } else if (a.y >= a.z) {
        face = d.y > 0 ? 2 : 3;
        sc = d.x;
        tc = d.y > 0 ? d.z : -d.z;
    } else {
        face = d.z > 0 ? 4 : 5;
        sc = d.x;
        tc = d.y > 0 ? -d.y : d.y;
    }
    // ma maps to [-1,1]
    float ma = [&] {
        switch (face) {
            case 0:
            case 1: return d.x;
            case 2:
            case 3: return d.y;
            default: return d.z;
        }
    }();
    sc /= ma;
    tc /= ma;
    uint32_t x = std::clamp<uint32_t>(static_cast<uint32_t>((sc * 0.5f + 0.5f) * size),
                                      0, size - 1);
    uint32_t y = std::clamp<uint32_t>(static_cast<uint32_t>((tc * 0.5f + 0.5f) * size),
                                      0, size - 1);
    const auto& f = faces[face];
    uint32_t o = (y * size + x) * 4;
    return {f[o] / 255.0f, f[o + 1] / 255.0f, f[o + 2] / 255.0f};
}

// Hammersley quasi-random sequence.
float radicalInverse(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

glm::vec3 hammersleySample(uint32_t i, uint32_t n) {
    float u1 = radicalInverse(i);
    float u2 = static_cast<float>(i + 0.5) / n;
    float r = std::sqrt(u2);
    float phi = u1 * 6.28318530718f;
    return {r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.0f, 1.0f - u2))};
}

void buildTangentFrame(const glm::vec3& n, glm::vec3& t, glm::vec3& b) {
    glm::vec3 up = std::abs(n.z) < 0.999f ? glm::vec3{0, 0, 1} : glm::vec3{1, 0, 0};
    t = glm::normalize(glm::cross(up, n));
    b = glm::cross(n, t);
}

// GGX NDF sampling around +z.
glm::vec3 importanceSampleGGX(float u1, float u2, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0f * PI_f * u1;
    float cosT = std::sqrt((1.0f - u2) / (1.0f + (a * a - 1.0f) * u2));
    float sinT = std::sqrt(1.0f - cosT * cosT);
    return {sinT * std::cos(phi), sinT * std::sin(phi), cosT};
}


} // namespace

glm::vec3 VulkanEnvironment::cubeDirection(uint32_t face, uint32_t x, uint32_t y, uint32_t size) {
    float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
    float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
    switch (face) {
        case 0: return {1.0f, -v, -u};
        case 1: return {-1.0f, -v, u};
        case 2: return {u, 1.0f, v};
        case 3: return {u, -1.0f, -v};
        case 4: return {u, -v, 1.0f};
        default: return {-u, -v, -1.0f};
    }
}

VulkanEnvironment::VulkanEnvironment(VulkanDevice& device,
                                     const VulkanSwapchain& swapchain,
                                     VkDescriptorSetLayout cameraLayout,
                                     VkDescriptorSetLayout materialLayout,
                                     VkDescriptorSetLayout shadowSamplerLayout)
    : device_(device), swapchain_(swapchain) {
    otherSetLayouts_ = {cameraLayout, materialLayout, shadowSamplerLayout};
    buildCubemaps();
    buildBrdfLut();
    createSamplersAndSet3();
    buildSkyboxPipelineAndMesh();
}

VulkanEnvironment::~VulkanEnvironment() {
    VkDevice dev = device_.handle();
    vkDeviceWaitIdle(dev);
    if (skyboxPipeline_) vkDestroyPipeline(dev, skyboxPipeline_, nullptr);
    if (skyboxPipelineLayout_)
        vkDestroyPipelineLayout(dev, skyboxPipelineLayout_, nullptr);
    if (skyboxVertexBuffer_) vkDestroyBuffer(dev, skyboxVertexBuffer_, nullptr);
    if (skyboxVertexMemory_) vkFreeMemory(dev, skyboxVertexMemory_, nullptr);
    if (skyboxIndexBuffer_) vkDestroyBuffer(dev, skyboxIndexBuffer_, nullptr);
    if (skyboxIndexMemory_) vkFreeMemory(dev, skyboxIndexMemory_, nullptr);
    if (set3Pool_) vkDestroyDescriptorPool(dev, set3Pool_, nullptr);
    auto destroyCube = [&](CubeTexture& t) {
        if (t.view) vkDestroyImageView(dev, t.view, nullptr);
        if (t.image) vkDestroyImage(dev, t.image, nullptr);
        if (t.memory) vkFreeMemory(dev, t.memory, nullptr);
    };
    destroyCube(env_);
    destroyCube(irradiance_);
    destroyCube(prefiltered_);
    if (brdfView_) vkDestroyImageView(dev, brdfView_, nullptr);
    if (brdfImage_) vkDestroyImage(dev, brdfImage_, nullptr);
    if (brdfMemory_) vkFreeMemory(dev, brdfMemory_, nullptr);
    if (envSampler_) vkDestroySampler(dev, envSampler_, nullptr);
    if (irrSampler_) vkDestroySampler(dev, irrSampler_, nullptr);
    if (prefSampler_) vkDestroySampler(dev, prefSampler_, nullptr);
    if (lutSampler_) vkDestroySampler(dev, lutSampler_, nullptr);
}

void VulkanEnvironment::generateEnvPixels(
    std::vector<std::vector<unsigned char>>& faces, uint32_t faceSize) {
    const glm::vec3 sunDir =
        glm::normalize(glm::vec3{0.35f, 0.55f, 0.76f}); // matches key light

    faces.resize(6);
    for (uint32_t f = 0; f < 6; ++f) {
        faces[f].resize(static_cast<size_t>(faceSize) * faceSize * 4);
        for (uint32_t y = 0; y < faceSize; ++y) {
            for (uint32_t x = 0; x < faceSize; ++x) {
                glm::vec3 d = glm::normalize(cubeDirection(f, x, y, faceSize));

                glm::vec3 c;
                if (d.y < 0.0f) {
                    // Ground: dark warm gray fading with distance below horizon.
                    float g = std::max(0.0f, 0.25f + d.y * 0.6f);
                    c = glm::vec3{0.28f * g + 0.03f, 0.25f * g + 0.03f,
                                  0.22f * g + 0.04f};
                } else {
                    // Sky: horizon haze -> deep zenith blue.
                    float t = std::pow(d.y, 0.45f);
                    c = glm::mix(glm::vec3{0.55f, 0.62f, 0.72f},
                                 glm::vec3{0.06f, 0.12f, 0.34f}, t);
                    // Sun disk + halo.
                    float s = std::max(0.0f, glm::dot(d, sunDir));
                    c += glm::vec3{1.0f, 0.92f, 0.72f} *
                         (std::pow(s, 350.0f) * kSunIntensity +
                          std::pow(s, 24.0f) * 0.22f);
                }

                uint32_t o = (y * faceSize + x) * 4;
                faces[f][o + 0] = static_cast<unsigned char>(
                    std::clamp(c.r, 0.0f, 1.0f) * 255.0f);
                faces[f][o + 1] = static_cast<unsigned char>(
                    std::clamp(c.g, 0.0f, 1.0f) * 255.0f);
                faces[f][o + 2] = static_cast<unsigned char>(
                    std::clamp(c.b, 0.0f, 1.0f) * 255.0f);
                faces[f][o + 3] = 255;
            }
        }
    }
}

// --- GPU upload helpers ------------------------------------------------------

constexpr uint32_t kFramesInFlight = 2;

namespace {

struct StagingUpload {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

std::string envExeDir() {
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return ".";
    std::string p(buf, static_cast<size_t>(len));
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

std::vector<char> envReadFileBytes(const std::string& path) {
    std::string exeDir = envExeDir();
    std::string full = path;
    for (const std::string& base : {exeDir + "/", exeDir + "/../"}) {
        FILE* probe = std::fopen((base + path).c_str(), "rb");
        if (probe) { std::fclose(probe); full = base + path; break; }
    }
    FILE* f = std::fopen(full.c_str(), "rb");
    if (!f) fatal("env shader open failed");
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::rewind(f);
    std::vector<char> bytes(static_cast<size_t>(size));
    if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size())
        fatal("env shader short read");
    std::fclose(f);
    return bytes;
}

VkShaderModule envCreateShaderModule(VkDevice dev, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS)
        fatal("env shader module create");
    return m;
}

StagingUpload makeStaging(const VulkanDevice& device, VkDeviceSize bytes) {
    StagingUpload s;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(device.handle(), &bi, nullptr, &s.buffer) != VK_SUCCESS)
        fatal("staging buffer create");
    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(device.handle(), s.buffer, &reqs);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(device.physical(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((reqs.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = reqs.size;
            ai.memoryTypeIndex = i;
            if (vkAllocateMemory(device.handle(), &ai, nullptr, &s.memory) == VK_SUCCESS)
                break;
            s.memory = VK_NULL_HANDLE;
        }
    }
    if (s.memory == VK_NULL_HANDLE) fatal("staging memory alloc");
    if (vkBindBufferMemory(device.handle(), s.buffer, s.memory, 0) != VK_SUCCESS)
        fatal("staging bind");
    return s;
}

} // namespace

void VulkanEnvironment::uploadCubeFaces(CubeTexture& tex, uint32_t size, uint32_t mipLevels,
                                        const std::vector<std::vector<unsigned char>>& facesRGBA8,
                                        const std::vector<std::vector<float>>* facesHDR16) {
    // Flatten all layers/mips into one staging blob.
    std::vector<VkBufferImageCopy> regions;
    VkDeviceSize offset = 0;
    std::vector<char> blob;

    uint32_t w = size, h = size;
    for (uint32_t mip = 0; mip < mipLevels; ++mip) {
        for (uint32_t layer = 0; layer < 6; ++layer) {
            VkBufferImageCopy r{};
            r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            r.imageSubresource.mipLevel = mip;
            r.imageSubresource.baseArrayLayer = layer;
            r.imageSubresource.layerCount = 1;
            r.imageExtent = {w, h, 1};
            r.bufferOffset = offset;
            regions.push_back(r);

            VkDeviceSize faceBytes = static_cast<VkDeviceSize>(w) * h * 4;
            blob.resize(blob.size() + static_cast<size_t>(faceBytes));
            const unsigned char* src = facesRGBA8.empty()
                                           ? nullptr
                                           : facesRGBA8[layer].data();
            if (facesHDR16) {
                // RGBA8 conversion from float HDR source.
                for (uint32_t p = 0; p < w * h; ++p) {
                    glm::vec4 v(*reinterpret_cast<const glm::vec4*>(&facesHDR16[layer][p * 4]));
                    unsigned char out[4] = {
                        static_cast<unsigned char>(std::clamp(v.r, 0.0f, 1.0f) * 255.0f),
                        static_cast<unsigned char>(std::clamp(v.g, 0.0f, 1.0f) * 255.0f),
                        static_cast<unsigned char>(std::clamp(v.b, 0.0f, 1.0f) * 255.0f), 255};
                    std::memcpy(blob.data() + offset + static_cast<size_t>(p) * 4, out, 4);
                }
            } else if (src) {
                std::memcpy(blob.data() + offset, src + static_cast<size_t>(mip) * 0 +
                                                      static_cast<size_t>(layer) * w * h * 4,
                            static_cast<size_t>(faceBytes));
            }
            offset += faceBytes;
        }
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }

    auto st = makeStaging(device_, static_cast<VkDeviceSize>(blob.size()));
    void* mapped = nullptr;
    vkMapMemory(device_.handle(), st.memory, 0, static_cast<VkDeviceSize>(blob.size()), 0,
                &mapped);
    std::memcpy(mapped, blob.data(), blob.size());
    vkUnmapMemory(device_.handle(), st.memory);

    VkCommandBufferAllocateInfo cbAlloc{};
    cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool = device_.commandPool();
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_.handle(), &cbAlloc, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.image = tex.image;
    bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.levelCount = mipLevels;
    bar.subresourceRange.layerCount = 6;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcAccessMask = 0;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    vkCmdCopyBufferToImage(cmd, st.buffer, tex.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &bar);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(device_.graphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(device_.graphicsQueue());
    vkFreeCommandBuffers(device_.handle(), device_.commandPool(), 1, &cmd);

    vkDestroyBuffer(device_.handle(), st.buffer, nullptr);
    vkFreeMemory(device_.handle(), st.memory, nullptr);
}

void VulkanEnvironment::createCubeImage(CubeTexture& out, uint32_t size, uint32_t mipLevels,
                                        VkFormat format,
                                        const std::vector<std::vector<unsigned char>>& facesRGBA8,
                                        const std::vector<std::vector<float>>* facesHDR16) {
    VkDevice dev = device_.handle();
    out.mipLevels = mipLevels;

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {size, size, 1};
    ii.mipLevels = mipLevels;
    ii.arrayLayers = 6;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(dev, &ii, nullptr, &out.image) != VK_SUCCESS)
        fatal("cube image create");

    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(dev, out.image, &reqs);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(device_.physical(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((reqs.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = reqs.size;
            ai.memoryTypeIndex = i;
            if (vkAllocateMemory(dev, &ai, nullptr, &out.memory) == VK_SUCCESS) break;
            out.memory = VK_NULL_HANDLE;
        }
    }
    if (out.memory == VK_NULL_HANDLE) fatal("cube memory alloc");
    vkBindImageMemory(dev, out.image, out.memory, 0);

    uploadCubeFaces(out, size, mipLevels, facesRGBA8, facesHDR16);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vi.format = format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = mipLevels;
    vi.subresourceRange.layerCount = 6;
    if (vkCreateImageView(dev, &vi, nullptr, &out.view) != VK_SUCCESS)
        fatal("cube view create");
}


void VulkanEnvironment::buildCubemaps() {
    // Source env.
    std::vector<std::vector<unsigned char>> envFaces;
    generateEnvPixels(envFaces, kEnvSize);
    createCubeImage(env_, kEnvSize, 1, VK_FORMAT_R8G8B8A8_SRGB, envFaces);

    // Irradiance: cosine-weighted convolution at low res.
    std::vector<std::vector<unsigned char>> irrFaces(6);
    for (uint32_t f = 0; f < 6; ++f)
        irrFaces[f].resize(static_cast<size_t>(kIrrSize) * kIrrSize * 4);

    constexpr uint32_t kSamples = 256;
    std::mt19937 rng(12345);
    for (uint32_t f = 0; f < 6; ++f) {
        for (uint32_t y = 0; y < kIrrSize; ++y) {
            for (uint32_t x = 0; x < kIrrSize; ++x) {
                glm::vec3 N = glm::normalize(cubeDirection(f, x, y, kIrrSize));
                glm::vec3 t, b;
                buildTangentFrame(N, t, b);
                glm::vec3 sum{0.0f};
                for (uint32_t s = 0; s < kSamples; ++s) {
                    float u1 = radicalInverse(s), u2 = radicalInverse(s * 2654435761u + 7u);
                    float cosT = std::sqrt(1.0f - u1);
                    float sinT = std::sqrt(u1);
                    float phi = 6.2831853f * u2;
                    glm::vec3 L = t * (sinT * std::cos(phi)) +
                                  b * (sinT * std::sin(phi)) + N * cosT;
                    sum += sampleEnvFaces(envFaces, kEnvSize, glm::normalize(L));
                }
                sum /= static_cast<float>(kSamples);
                uint32_t o = (y * kIrrSize + x) * 4;
                irrFaces[f][o + 0] =
                    static_cast<unsigned char>(std::clamp(sum.r, 0.0f, 1.0f) * 255.0f);
                irrFaces[f][o + 1] =
                    static_cast<unsigned char>(std::clamp(sum.g, 0.0f, 1.0f) * 255.0f);
                irrFaces[f][o + 2] =
                    static_cast<unsigned char>(std::clamp(sum.b, 0.0f, 1.0f) * 255.0f);
                irrFaces[f][o + 3] = 255;
            }
        }
    }
    createCubeImage(irradiance_, kIrrSize, 1, VK_FORMAT_R8G8B8A8_SRGB, irrFaces);

    // Prefiltered: GGX importance-sampled mips. Mip r -> roughness r/(count-1).
    constexpr uint32_t kPrefMips = 6;
    std::vector<std::vector<unsigned char>> prefFaces(6);
    for (uint32_t mip = 0; mip < kPrefMips; ++mip) {
        float roughness = mip == 0 ? 0.08f
                                   : static_cast<float>(mip) / (kPrefMips - 1);
        roughness = std::clamp(roughness, 0.05f, 1.0f);
        uint32_t size = kPrefSize >> mip;

        for (uint32_t f = 0; f < 6; ++f)
            prefFaces[f].assign(static_cast<size_t>(size) * size * 4, 255);

        constexpr uint32_t kSpp = 48;
        for (uint32_t f = 0; f < 6; ++f) {
            for (uint32_t y = 0; y < size; ++y) {
                for (uint32_t x = 0; x < size; ++x) {
                    glm::vec3 N = glm::normalize(cubeDirection(f, x, y, size));
                    glm::vec3 V = N;
                    glm::vec3 t, b;
                    buildTangentFrame(N, t, b);
                    glm::vec3 sum{0.0f};
                    for (uint32_t s = 0; s < kSpp; ++s) {
                        glm::vec3 H = importanceSampleGGX(radicalInverse(s),
                                                          radicalInverse(s * 97u + 13u),
                                                          roughness);
                        glm::vec3 L = glm::normalize(2.0f * dot(V, H) * H - V);
                        float nDotL = std::max(dot(N, L), 0.0f);
                        if (nDotL > 0.0f)
                            sum += sampleEnvFaces(envFaces, kEnvSize, L) * nDotL;
                    }
                    sum /= static_cast<float>(kSpp);
                    uint32_t o = (y * size + x) * 4;
                    prefFaces[f][o + 0] =
                        static_cast<unsigned char>(std::clamp(sum.r, 0.0f, 1.0f) * 255.0f);
                    prefFaces[f][o + 1] =
                        static_cast<unsigned char>(std::clamp(sum.g, 0.0f, 1.0f) * 255.0f);
                    prefFaces[f][o + 2] =
                        static_cast<unsigned char>(std::clamp(sum.b, 0.0f, 1.0f) * 255.0f);
                    prefFaces[f][o + 3] = 255;
                }
            }
        }
    }
    createCubeImage(prefiltered_, kPrefSize, kPrefMips, VK_FORMAT_R8G8B8A8_SRGB,
                    prefFaces);
}

float lutGeometrySmith(const glm::vec3& N, const glm::vec3& V, const glm::vec3& L,
                       float roughness) {
    float NdotV = std::max(glm::dot(N, V), 0.001f);
    float NdotL = std::max(glm::dot(L, N), 0.001f);
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    auto ggx = [&](float x) { return x / (x * (1.0f - k) + k); };
    return ggx(NdotV) * ggx(NdotL);
}

void VulkanEnvironment::buildBrdfLut() {
    VkDevice dev = device_.handle();
    constexpr uint32_t S = kBrdfSize;

    std::vector<float> pixels(static_cast<size_t>(S) * S * 2);
    constexpr uint32_t kSpp = 64;
    for (uint32_t y = 0; y < S; ++y) {
        float roughness = (static_cast<float>(y) + 0.5f) / S;
        for (uint32_t x = 0; x < S; ++x) {
            float NoV = (static_cast<float>(x) + 0.5f) / S;
            NoV = std::max(NoV, 1e-4f);
            glm::vec3 V{std::sqrt(1.0f - NoV * NoV), 0.0f, NoV};
            glm::vec3 N{0.0f, 0.0f, 1.0f};

            using glm::clamp;
            glm::vec2 sum{0.0f};
            for (uint32_t s = 0; s < kSpp; ++s) {
                glm::vec3 H = importanceSampleGGX(radicalInverse(s),
                                                  radicalInverse(s * 131u + 5u),
                                                  roughness);
                glm::vec3 L = glm::normalize(2.0f * dot(N, H) * H - V);
                float nDotL = L.z, nDotV = V.z;
                if (nDotL > 0.0f && nDotV > 0.0f) {
                    glm::vec3 hv = glm::normalize(H + V);
                    float vDotH = std::clamp(glm::dot(V, hv), 0.0f, 1.0f);
                    float g = lutGeometrySmith(N, V, L, roughness);
                    float f = std::pow(1.0f - vDotH, 5.0f); // F0 = 1 -> F = f
                    sum += glm::vec2((1.0f - f) * g, f) * nDotL /
                           std::max(glm::dot(N, H), 1e-4f);
                }
            }
            sum /= static_cast<float>(kSpp);
            uint32_t o = (y * S + x) * 2;
            pixels[o + 0] = sum.x;
            pixels[o + 1] = sum.y;
        }
    }

    // Upload as RG16F.
    VkDeviceSize bytes = static_cast<VkDeviceSize>(S) * S * 2 * sizeof(uint16_t);
    auto st = makeStaging(device_, bytes);
    void* mapped = nullptr;
    vkMapMemory(device_.handle(), st.memory, 0, bytes, 0, &mapped);
    std::vector<uint16_t> halfs(static_cast<size_t>(S) * S * 2);
    for (size_t i = 0; i < halfs.size(); ++i) {
        float v = i % 2 == 0 ? pixels[i] : pixels[i];
        // float->half conversion via bit trick (fast enough at startup).
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        uint16_t h10 = ((bits >> 16) & 0x8000) |
                       ((((bits & 0x7F800000) - 0x38000000) >> 13) & 0x7FFF);
        halfs[i] = h10;
    }
    std::memcpy(mapped, halfs.data(), halfs.size() * 2);
    vkUnmapMemory(device_.handle(), st.memory);

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16_SFLOAT;
    ii.extent = {S, S, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(dev, &ii, nullptr, &brdfImage_) != VK_SUCCESS)
        fatal("brdf image create");

    VkMemoryRequirements reqs{};
    vkGetImageMemoryRequirements(dev, brdfImage_, &reqs);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(device_.physical(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((reqs.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = reqs.size;
            ai.memoryTypeIndex = i;
            if (vkAllocateMemory(dev, &ai, nullptr, &brdfMemory_) == VK_SUCCESS) break;
            brdfMemory_ = VK_NULL_HANDLE;
        }
    }
    if (brdfMemory_ == VK_NULL_HANDLE) fatal("brdf memory alloc");
    vkBindImageMemory(dev, brdfImage_, brdfMemory_, 0);

    VkCommandBufferAllocateInfo cbAlloc{};
    cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool = device_.commandPool();
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev, &cbAlloc, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.image = brdfImage_;
    bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.levelCount = 1;
    bar.subresourceRange.layerCount = 1;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {S, S, 1};
    vkCmdCopyBufferToImage(cmd, st.buffer, brdfImage_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &bar);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(device_.graphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(device_.graphicsQueue());
    vkFreeCommandBuffers(dev, device_.commandPool(), 1, &cmd);

    vkDestroyBuffer(device_.handle(), st.buffer, nullptr);
    vkFreeMemory(device_.handle(), st.memory, nullptr);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = brdfImage_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16G16_SFLOAT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(dev, &vi, nullptr, &brdfView_) != VK_SUCCESS)
        fatal("brdf view create");
}


void VulkanEnvironment::createSamplersAndSet3() {
    VkDevice dev = device_.handle();

    auto makeSampler = [&](bool mips, bool edge) {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = mips ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                             : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW =
            edge ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                 : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        si.maxLod = mips ? VK_LOD_CLAMP_NONE : 0.0f;
        VkSampler s = VK_NULL_HANDLE;
        if (vkCreateSampler(dev, &si, nullptr, &s) != VK_SUCCESS)
            fatal("sampler create");
        return s;
    };

    irrSampler_ = makeSampler(false, true);
    prefSampler_ = makeSampler(true, true);
    lutSampler_ = makeSampler(false, true);
    envSampler_ = makeSampler(false, false);

    set3Layout_ = device_.set3Layout();

    VkDescriptorPoolSize sizes[4]{};
    for (auto& p : sizes) {
        p.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        p.descriptorCount = 64; // ample headroom for all sets
    }
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 8;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &set3Pool_) != VK_SUCCESS)
        fatal("set3 pool create");

    frameSets_.resize(8);
    for (auto& set : frameSets_) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = set3Pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set3Layout_;
        if (vkAllocateDescriptorSets(device_.handle(), &ai, &set) != VK_SUCCESS)
            fatal("set3 alloc");

        VkDescriptorImageInfo imgs[4] = {
            {irrSampler_, irradiance_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {prefSampler_, prefiltered_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {lutSampler_, brdfView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {envSampler_, env_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};

        VkWriteDescriptorSet writes[4]{};
        for (uint32_t b = 0; b < 4; ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = set;
            writes[b].dstBinding = b;
            writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].descriptorCount = 1;
            writes[b].pImageInfo = &imgs[b];
        }
        vkUpdateDescriptorSets(dev, 4, writes, 0, nullptr);
    }
}

void VulkanEnvironment::buildSkyboxPipelineAndMesh() {
    VkDevice dev = device_.handle();
    createSkyboxPipeline(swapchain_.renderPass(), otherSetLayouts_[0],
                         otherSetLayouts_[1], otherSetLayouts_[2]);

    // Unit cube, non-indexed (36 verts).
    const float cubeVerts[36 * 3] = {
        -1,-1,-1, -1,-1, 1, -1, 1, 1,   -1,-1,-1, -1, 1, 1, -1, 1,-1,
         1, 1, 1,  1,-1, 1,  1,-1,-1,    1, 1, 1,  1,-1,-1,  1, 1,-1,
        -1, 1, 1, -1,-1, 1,  1,-1, 1,   -1, 1, 1,  1,-1, 1,  1, 1, 1,
         1, 1,-1,  1,-1,-1, -1,-1,-1,    1, 1,-1, -1,-1,-1, -1, 1,-1,
        -1,-1, 1,  1,-1, 1,  1,-1,-1,   -1,-1, 1,  1,-1,-1, -1,-1,-1,
        -1, 1,-1,  1, 1,-1,  1, 1, 1,   -1, 1,-1,  1, 1, 1, -1, 1, 1,
    };

    VkDeviceSize vbBytes = sizeof(cubeVerts);

    VkBufferCreateInfo vbi{};
    vbi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vbi.size = vbBytes;
    vbi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (vkCreateBuffer(dev, &vbi, nullptr, &skyboxVertexBuffer_) != VK_SUCCESS)
        fatal("skybox vb");
    VkMemoryRequirements reqs{};
    vkGetBufferMemoryRequirements(dev, skyboxVertexBuffer_, &reqs);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(device_.physical(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((reqs.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = reqs.size;
            ai.memoryTypeIndex = i;
            if (vkAllocateMemory(dev, &ai, nullptr, &skyboxVertexMemory_) == VK_SUCCESS)
                break;
            skyboxVertexMemory_ = VK_NULL_HANDLE;
        }
    }
    if (!skyboxVertexMemory_) fatal("skybox vb mem");
    vkBindBufferMemory(dev, skyboxVertexBuffer_, skyboxVertexMemory_, 0);
    void* data = nullptr;
    vkMapMemory(dev, skyboxVertexMemory_, 0, vbBytes, 0, &data);
    std::memcpy(data, cubeVerts, vbBytes);
    vkUnmapMemory(dev, skyboxVertexMemory_);
}

void VulkanEnvironment::beginSkybox(VkCommandBuffer cmd, uint32_t frameIndex,
                                    const glm::mat4& viewProjection,
                                    const glm::vec3& cameraPos) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyboxPipeline_);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(swapchain_.width()),
                  static_cast<float>(swapchain_.height()), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {swapchain_.width(), swapchain_.height()}};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    struct Push {
        glm::mat4 vp;
        glm::vec4 cam;
    } push{viewProjection, glm::vec4(cameraPos, 1.0f)};
    vkCmdPushConstants(cmd, skyboxPipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(Push), &push);

    VkDescriptorSet set = frameSets_[frameIndex];
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            skyboxPipelineLayout_, 3, 1, &set, 0, nullptr);
}

void VulkanEnvironment::drawSkybox(VkCommandBuffer cmd) {
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &skyboxVertexBuffer_, &offset);
    vkCmdDraw(cmd, 36, 1, 0, 0);
}

void VulkanEnvironment::endSkybox(VkCommandBuffer) {}

// Pipeline created inside buildSkyboxPipelineAndMesh continuation.
void VulkanEnvironment::createSkyboxPipeline(
    VkRenderPass renderPass, VkDescriptorSetLayout cameraLayout,
    VkDescriptorSetLayout materialLayout,
    VkDescriptorSetLayout shadowSamplerLayout) {
    VkDevice dev = device_.handle();

    auto vertCode = envReadFileBytes("shaders/skybox.vert.spv");
    auto fragCode = envReadFileBytes("shaders/skybox.frag.spv");
    VkShaderModule vert = envCreateShaderModule(dev, vertCode);
    VkShaderModule frag = envCreateShaderModule(dev, fragCode);

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset = 0;
    push.size = sizeof(glm::mat4) + sizeof(glm::vec4);

    std::array<VkDescriptorSetLayout, 4> skySets = {
        cameraLayout, materialLayout, shadowSamplerLayout, set3Layout_};
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = static_cast<uint32_t>(skySets.size());
    li.pSetLayouts = skySets.data();
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &li, nullptr, &skyboxPipelineLayout_) != VK_SUCCESS)
        fatal("skybox layout create");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{0, sizeof(glm::vec3),
                                            VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription posAttr{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions = &posAttr;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    const VkDynamicState skyboxDynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                             VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo skyboxDynamic{};
    skyboxDynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    skyboxDynamic.dynamicStateCount = 2;
    skyboxDynamic.pDynamicStates = skyboxDynamics;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE; // draw all cube faces from inside

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;   // occluded by scene
    depthStencil.depthWriteEnable = VK_FALSE; // never write depth
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

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
    pipeInfo.pColorBlendState = &colorBlend;
    pipeInfo.pDynamicState = &skyboxDynamic;
    pipeInfo.layout = skyboxPipelineLayout_;
    pipeInfo.renderPass = renderPass;
    pipeInfo.subpass = 0;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipeInfo, nullptr,
                                  &skyboxPipeline_) != VK_SUCCESS)
        fatal("skybox pipeline create");

    vkDestroyShaderModule(dev, vert, nullptr);
    vkDestroyShaderModule(dev, frag, nullptr);
}
} // namespace engine
