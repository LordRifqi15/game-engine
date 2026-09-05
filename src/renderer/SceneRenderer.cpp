#include "renderer/SceneRenderer.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unistd.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "renderer/culling/HiZPyramid.hpp"
#include "renderer/meshlet/MeshletBuilder.hpp"
#include "renderer/vulkan/VkDeviceMemoryHelper.hpp"
#include "renderer/vulkan/PhysicalImage.hpp"
#include "renderer/vulkan/environment.h"
#include "renderer/vulkan/shadow_pass.h"
#include "renderer/vulkan/texture_cache.h"
#include "renderer/vulkan/vk_device.h"
#include "renderer/vulkan/vk_swapchain.h"

namespace Engine {
namespace {

// Must match gbuffer_bindless.vert / legacy basic.vert set 0 binding 0.
struct CameraBlock {
    glm::mat4 viewProjection{1.0f};
    glm::vec4 cameraPos{0.0f};
    glm::vec4 lightDir{0.0f, -1.0f, 0.0f, 0.0f};
    glm::vec4 lightColor{1.0f};
    glm::vec4 params{0.25f, 0.0f, 0.0f, 0.0f};
    glm::mat4 lightVP{1.0f};
};

// Must match deferred_lighting.frag set 2 binding 0.
struct DeferredFrameBlock {
    glm::mat4 invViewProj{1.0f};
    glm::mat4 view{1.0f};
    glm::vec4 cameraPos{0.0f};
    glm::vec4 dirLightDir{0.0f, -1.0f, 0.0f, 0.0f};
    glm::vec4 dirLightColor{1.0f, 1.0f, 1.0f, 1.0f};
    glm::mat4 shadowVP{1.0f};
    glm::vec4 shadowParams{1.0f / 1024.0f, 1.0f, 1024.0f, 0.0f};
};

struct ClusterCamBlock {
    glm::mat4 view{1.0f};
    glm::mat4 invProj{1.0f};
};

struct MeshletCamBlock {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::vec4 frustum[6]{};
};

// Meshlet vertex pool format (must match gbuffer_meshlet.vert Vertex).
struct MeshletVertex {
    glm::vec4 position{0.0f};
    glm::vec4 normal{0.0f};
    glm::vec2 uv{0.0f};
    glm::vec2 padding{0.0f}; // x: bindless material ID
};

// Classic indexed draw instance (matches legacy InstanceData layout).
struct ClassicInstance {
    glm::mat4 model{1.0f};
    glm::vec4 color{1.0f};
    glm::vec4 params{0.0f, 1.0f, 0.0f, 0.0f};
};

std::vector<char> readShaderBytes(const char* name) {
    char exeBuf[4096];
    std::string exeDir = ".";
    ssize_t len = ::readlink("/proc/self/exe", exeBuf, sizeof(exeBuf) - 1);
    if (len > 0) {
        exeBuf[len] = '\0';
        std::string exe(exeBuf);
        auto slash = exe.find_last_of('/');
        if (slash != std::string::npos) exeDir = exe.substr(0, slash);
    }
    const std::string candidates[] = {
        "build/shaders/" + std::string(name),
        exeDir + "/shaders/" + name,
        exeDir + "/../shaders/" + name,
        "shaders/" + std::string(name),
    };
    for (auto& p : candidates) {
        std::ifstream f(p, std::ios::ate | std::ios::binary);
        if (!f) continue;
        size_t size = static_cast<size_t>(f.tellg());
        if (size == 0) continue;
        std::vector<char> bytes(size);
        f.seekg(0);
        if (f.read(bytes.data(), size)) {
            uint32_t checksum = 0;
            for (size_t i = 0; i + 3 < size; i += 4) {
                uint32_t w = 0;
                std::memcpy(&w, bytes.data() + i, 4);
                checksum = checksum * 31 + w;
            }
            std::fprintf(stderr, "[shader] %s <- %s (%zu bytes, chk=%08x)\n", name, p.c_str(), size,
                         checksum);
            return bytes;
        }
    }
    throw std::runtime_error(std::string("open shader: ") + name);
}

void extractFrustum(const glm::mat4& vp, glm::vec4 planes[6]) {
    // Rows of VP; planes point inward.
    glm::vec4 row0(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    glm::vec4 row1(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    glm::vec4 row2(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    glm::vec4 row3(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);
    planes[0] = row3 + row0; // left
    planes[1] = row3 - row0; // right
    planes[2] = row3 + row1; // bottom
    planes[3] = row3 - row1; // top
    planes[4] = row3 + row2; // near
    planes[5] = row3 - row2; // far
    for (int i = 0; i < 6; ++i) {
        float l = glm::length(glm::vec3(planes[i]));
        if (l > 0.0f) planes[i] /= l;
    }
}

} // namespace

bool SceneRenderer::createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, HostBuffer& out) {
    destroyHostBuffer(out);
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = std::max<VkDeviceSize>(size, 16);
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &ci, nullptr, &out.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, out.buffer, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    try {
        ai.memoryTypeIndex = findMemoryType(physical_, req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    } catch (...) {
        vkDestroyBuffer(device_, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkAllocateMemory(device_, &ai, nullptr, &out.memory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(device_, out.buffer, out.memory, 0);
    vkMapMemory(device_, out.memory, 0, req.size, 0, &out.mapped);
    out.size = req.size;
    return true;
}

void SceneRenderer::destroyHostBuffer(HostBuffer& buf) {
    if (device_ == VK_NULL_HANDLE) {
        buf = HostBuffer{};
        return;
    }
    if (buf.mapped) vkUnmapMemory(device_, buf.memory);
    if (buf.buffer) vkDestroyBuffer(device_, buf.buffer, nullptr);
    if (buf.memory) vkFreeMemory(device_, buf.memory, nullptr);
    buf = HostBuffer{};
}

VkShaderModule SceneRenderer::loadShader(const char* name) {
    std::vector<char> code = readShaderBytes(name);
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &ci, nullptr, &m) != VK_SUCCESS)
        throw std::runtime_error(std::string("shader module: ") + name);
    return m;
}

bool SceneRenderer::init(VkDevice device, VkPhysicalDevice physical, VkQueue graphicsQueue,
                         uint32_t graphicsFamily, VkFormat swapchainFormat,
                         ::engine::VulkanDevice& legacyDevice, ::engine::VulkanSwapchain& swapchain,
                         ::engine::TextureCache& textures, ::engine::VulkanEnvironment& environment) {
    if (ready_) shutdown();
    device_ = device;
    physical_ = physical;
    graphicsQueue_ = graphicsQueue;
    graphicsFamily_ = graphicsFamily;
    swapchainFormat_ = swapchainFormat;
    legacyDevice_ = &legacyDevice;
    legacySwapchain_ = &swapchain;
    textures_ = &textures;
    environment_ = &environment;
    if (device_ == VK_NULL_HANDLE) return false;

    shadowPass_ = new ::engine::VulkanShadowPass(legacyDevice, swapchain);

    constexpr VkDeviceSize instBytes = kMaxDraws * sizeof(ClassicInstance);
    constexpr VkDeviceSize jointBytes = kMaxJoints * sizeof(glm::mat4);
    constexpr VkDeviceSize lightBytes = kMaxLights * sizeof(GPULight);
    for (uint32_t i = 0; i < kFrames; ++i) {
        if (!createHostBuffer(instBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              instanceRings_[i]))
            return false;
        if (!createHostBuffer(jointBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, jointRings_[i])) return false;
        if (!createHostBuffer(sizeof(CameraBlock), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, cameraRings_[i]))
            return false;
        if (!createHostBuffer(sizeof(DeferredFrameBlock), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, frameUboRings_[i]))
            return false;
        if (!createHostBuffer(lightBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, lightRings_[i])) return false;
        if (!createHostBuffer(sizeof(ClusterCamBlock), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, cullCamRings_[i]))
            return false;
        if (!createHostBuffer(sizeof(MeshletCamBlock), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, meshletCamRings_[i]))
            return false;
    }
    constexpr VkDeviceSize gridBytes = kGridXCap * kGridYCap * kClusterZ * sizeof(ClusterCell);
    constexpr VkDeviceSize indexBytes = sizeof(uint32_t) + kIndexListCap * sizeof(uint32_t);
    for (uint32_t i = 0; i < kFrames; ++i) {
        if (!createHostBuffer(gridBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, clusterGrid_[i]))
            return false;
        if (!createHostBuffer(indexBytes,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              clusterIndex_[i]))
            return false;
        if (!createHostBuffer(kCompactedCap * sizeof(uint32_t),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              compacted_[i]))
            return false;
        if (!createHostBuffer(sizeof(VkDrawIndexedIndirectCommand),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                              indirect_[i]))
            return false;
        if (!createHostBuffer((kMaxPages + 1) * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              residency_[i]))
            return false;
        if (!createHostBuffer((kRequestCap + 1) * sizeof(uint32_t),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              requests_[i]))
            return false;
        // Residency: page 0 (root) permanently resident; bake marks the rest.
        static_cast<uint32_t*>(residency_[i].mapped)[0] = 2;
    }

    bindless_.init(device_, physical_);
    // Fallback textures in reserved slots 0..2 (white, flat normal, neutral MR).
    const unsigned char white[4] = {255, 255, 255, 255};
    const unsigned char normalUp[4] = {128, 128, 255, 255};
    const unsigned char neutralMR[4] = {0, 255, 0, 255};
    const ::engine::Texture* fallbacks[3] = {
        textures_->createFromPixels("__modern_white", white, 1, 1),
        textures_->createFromPixels("__modern_normal", normalUp, 1, 1),
        textures_->createFromPixels("__modern_mr", neutralMR, 1, 1),
    };
    for (uint32_t i = 0; i < 3; ++i) {
        if (!fallbacks[i] || fallbacks[i]->view == VK_NULL_HANDLE) return false;
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = fallbacks[i]->view;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = bindless_.getDescriptorSet();
        w.dstBinding = 2;
        w.dstArrayElement = i;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.descriptorCount = 1;
        w.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }
    // Unit-cube for the skybox draw (positions only, matches skybox.vert).
    {
        const float s = 1.0f;
        const glm::vec3 verts[8] = {{-s, -s, -s}, {s, -s, -s}, {s, s, -s}, {-s, s, -s},
                                    {-s, -s, s},  {s, -s, s},  {s, s, s},  {-s, s, s}};
        const uint32_t idx[36] = {0, 1, 2, 2, 3, 0, 4, 6, 5, 6, 4, 7, 0, 4, 1, 1, 4, 5,
                                  2, 6, 3, 3, 6, 7, 0, 3, 4, 4, 3, 7, 1, 5, 2, 2, 5, 6};
        if (!createHostBuffer(sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, skyboxVerts_) ||
            !createHostBuffer(sizeof(idx), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, skyboxIndices_)) {
            shutdown();
            return false;
        }
        std::memcpy(skyboxVerts_.mapped, verts, sizeof(verts));
        std::memcpy(skyboxIndices_.mapped, idx, sizeof(idx));
    }

    if (!createDescriptorPool() || !createLayouts() || !createStaticDescriptors() ||
        !createSamplers() || !createPipelines(swapchainFormat)) {
        shutdown();
        return false;
    }
    ensureHistory({1, 1});
    ready_ = true;
    return true;
}

void SceneRenderer::shutdown() {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
    destroyPipelines();
    destroyDescriptors();
    if (device_ != VK_NULL_HANDLE && pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    bindless_.shutdown();
    for (auto& b : instanceRings_) destroyHostBuffer(b);
    for (auto& b : jointRings_) destroyHostBuffer(b);
    for (auto& b : cameraRings_) destroyHostBuffer(b);
    for (auto& b : frameUboRings_) destroyHostBuffer(b);
    for (auto& b : lightRings_) destroyHostBuffer(b);
    for (auto& b : cullCamRings_) destroyHostBuffer(b);
    for (auto& b : meshletCamRings_) destroyHostBuffer(b);
    for (auto& b : clusterGrid_) destroyHostBuffer(b);
    for (auto& b : clusterIndex_) destroyHostBuffer(b);
    for (auto& b : compacted_) destroyHostBuffer(b);
    for (auto& b : indirect_) destroyHostBuffer(b);
    for (auto& b : residency_) destroyHostBuffer(b);
    for (auto& b : requests_) destroyHostBuffer(b);
    destroyHostBuffer(vertexPool_);
    destroyHostBuffer(meshletPool_);
    destroyHostBuffer(uniquePool_);
    destroyHostBuffer(triPool_);
    for (auto& [mesh, bufs] : meshCache_) {
        destroyHostBuffer(bufs.verts);
        destroyHostBuffer(bufs.indices);
    }
    meshCache_.clear();
    destroyHostBuffer(skyboxVerts_);
    destroyHostBuffer(skyboxIndices_);
    for (uint32_t i = 0; i < kFrames; ++i) {
        for (auto v : hizViews_[i]) vkDestroyImageView(device_, v, nullptr);
        hizViews_[i].clear();
    }
    for (uint32_t i = 0; i < 2; ++i) {
        if (historyViews_[i] != VK_NULL_HANDLE) vkDestroyImageView(device_, historyViews_[i], nullptr);
        if (historyImages_[i] != VK_NULL_HANDLE) vkDestroyImage(device_, historyImages_[i], nullptr);
        if (historyMemories_[i] != VK_NULL_HANDLE) vkFreeMemory(device_, historyMemories_[i], nullptr);
        historyViews_[i] = VK_NULL_HANDLE;
        historyImages_[i] = VK_NULL_HANDLE;
        historyMemories_[i] = VK_NULL_HANDLE;
    }
    delete shadowPass_;
    shadowPass_ = nullptr;
    device_ = VK_NULL_HANDLE;
    ready_ = false;
}

SceneRenderer::MeshBuffers& SceneRenderer::meshBuffers(const ::engine::Mesh* mesh) {
    auto it = meshCache_.find(mesh);
    if (it != meshCache_.end()) return it->second;
    MeshBuffers bufs;
    VkDeviceSize vb = std::max<size_t>(mesh->vertices.size() * sizeof(::engine::Vertex), 16);
    VkDeviceSize ib = std::max<size_t>(mesh->indices.size() * sizeof(uint32_t), 16);
    if (!createHostBuffer(vb, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, bufs.verts) ||
        !createHostBuffer(ib, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, bufs.indices)) {
        throw std::runtime_error("mesh buffer alloc failed");
    }
    std::memcpy(bufs.verts.mapped, mesh->vertices.data(), mesh->vertices.size() * sizeof(::engine::Vertex));
    std::memcpy(bufs.indices.mapped, mesh->indices.data(), mesh->indices.size() * sizeof(uint32_t));
    bufs.indexCount = static_cast<uint32_t>(mesh->indices.size());
    return meshCache_.emplace(mesh, std::move(bufs)).first->second;
}

void SceneRenderer::ensureStaticCapacity(VkDeviceSize verts, VkDeviceSize meshlets, VkDeviceSize uniq,
                                         VkDeviceSize tris, uint32_t pages) {
    auto grow = [&](HostBuffer& buf, VkDeviceSize bytes, VkBufferUsageFlags usage) {
        if (buf.size >= bytes && buf.buffer != VK_NULL_HANDLE) return;
        VkDeviceSize grown = std::max(bytes, buf.size ? buf.size * 2 : 65536);
        if (!createHostBuffer(grown, usage, buf)) throw std::runtime_error("static pool grow failed");
        // Re-bake everything into the new pool next upload.
        lastStaticModels_.clear();
    };
    grow(vertexPool_, std::max<VkDeviceSize>(verts * sizeof(MeshletVertex), 16),
         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    grow(meshletPool_, std::max<VkDeviceSize>(meshlets * sizeof(GPUMeshlet), 16),
         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    grow(uniquePool_, std::max<VkDeviceSize>(uniq * sizeof(uint32_t), 16),
         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    grow(triPool_, std::max<VkDeviceSize>(tris * sizeof(uint32_t), 16),
         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    (void)pages;
}

bool SceneRenderer::staticsDirty(const GPUScene& scene) const {
    size_t statics = 0;
    for (auto& d : scene.draws) statics += d.skinned ? 0 : 1;
    if (statics != lastStaticModels_.size()) return true;
    size_t i = 0;
    for (auto& d : scene.draws) {
        if (d.skinned) continue;
        if (std::memcmp(&lastStaticModels_[i++], &d.model, sizeof(glm::mat4)) != 0) return true;
    }
    return false;
}

void SceneRenderer::bakeStatics(const GPUScene& scene) {
    // Count first so pools grow once.
    VkDeviceSize vertTotal = 0, uniqTotal = 0, triTotal = 0, meshletTotal = 0;
    uint32_t pageCount = 0;
    for (auto& d : scene.draws) {
        if (d.skinned || !d.mesh || d.mesh->empty()) continue;
        vertTotal += d.mesh->vertices.size();
        triTotal += d.mesh->indices.size(); // expanded to u32 below (<= 3x packed bytes)
        uniqTotal += d.mesh->vertices.size();
        meshletTotal += d.mesh->indices.size() / 3 / 8 + 1; // rough upper bound
        ++pageCount;
    }
    ensureStaticCapacity(vertTotal + 1, meshletTotal + 1, uniqTotal + 1, triTotal + 1, pageCount + 1);

    auto* vPool = static_cast<MeshletVertex*>(vertexPool_.mapped);
    auto* mPool = static_cast<GPUMeshlet*>(meshletPool_.mapped);
    auto* uPool = static_cast<uint32_t*>(uniquePool_.mapped);
    auto* tPool = static_cast<uint32_t*>(triPool_.mapped);
    VkDeviceSize vBase = 0, uBase = 0, tBase = 0, mBase = 0;
    uint32_t pageID = 1; // page 0 = pinned empty root
    lastStaticModels_.clear();
    bakedMeshlets_ = 0;
    bakedTris_ = 0;

    for (size_t di = 0; di < scene.draws.size(); ++di) {
        const auto& d = scene.draws[di];
        if (d.skinned || !d.mesh || d.mesh->empty()) continue;
        const uint32_t bindlessID = di < drawMaterial_.size() ? drawMaterial_[di] : 0;
        lastStaticModels_.push_back(d.model);
        const auto& mesh = *d.mesh;
        glm::mat3 normalMat(d.model);
        // Bake world-space vertices with material ID in padding.x.
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            const auto& v = mesh.vertices[i];
            MeshletVertex& o = vPool[vBase + i];
            o.position = glm::vec4(glm::vec3(d.model * glm::vec4(v.position, 1.0f)), 0.0f);
            o.normal = glm::vec4(glm::normalize(normalMat * v.normal), 0.0f);
            o.uv = v.uv;
            o.padding = glm::vec2(static_cast<float>(bindlessID), 0.0f);
        }
        std::vector<glm::vec3> positions(mesh.vertices.size());
        std::vector<glm::vec3> normals(mesh.vertices.size());
        std::vector<uint32_t> indices = mesh.indices;
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            positions[i] = vPool[vBase + i].position;
            normals[i] = vPool[vBase + i].normal;
        }
        std::vector<uint32_t> unique;
        std::vector<uint8_t> packed;
        std::vector<GPUMeshlet> meshlets =
            ::Engine::MeshletBuilder::buildMeshletsWithNormals(positions, normals, indices, unique, packed);
        // Append with rebased offsets; page ID rides in padding[0].
        for (size_t i = 0; i < unique.size(); ++i) uPool[uBase + i] = static_cast<uint32_t>(vBase) + unique[i];
        for (size_t i = 0; i < packed.size(); ++i) tPool[tBase + i] = packed[i];
        for (auto& m : meshlets) {
            m.vertexOffset += static_cast<uint32_t>(uBase);
            m.triangleOffset += static_cast<uint32_t>(tBase);
            m.padding[0] = pageID;
            mPool[mBase++] = m;
            bakedTris_ += m.triangleCount;
        }
        bakedMeshlets_ += static_cast<uint32_t>(meshlets.size());
        for (uint32_t s = 0; s < kFrames; ++s) {
            static_cast<uint32_t*>(residency_[s].mapped)[pageID] = 2; // resident after bake
        }
        ++pageID;
        vBase += mesh.vertices.size();
        uBase += unique.size();
        tBase += packed.size();
    }
    bakedPages_ = pageID;
    stats_.meshlets = bakedMeshlets_;
    stats_.triangles = bakedTris_;
    stats_.residentPages = pageID - 1;
    stats_.totalPages = pageID;
    for (uint32_t s = 0; s < kFrames; ++s) {
        static_cast<uint32_t*>(residency_[s].mapped)[0] = 2;
    }
    if (std::getenv("ENGINE_READBACK")) {
        auto* m0 = static_cast<GPUMeshlet*>(meshletPool_.mapped);
        auto* v0 = static_cast<MeshletVertex*>(vertexPool_.mapped);
        std::fprintf(stderr,
                     "[bake] meshlets=%u pages=%u v0=(%.2f,%.2f,%.2f) m0[c=(%.1f,%.1f,%.1f,r=%.1f) "
                     "vOff=%u vCnt=%u tOff=%u tCnt=%u page=%u]\n",
                     stats_.meshlets, pageID, v0->position.x, v0->position.y, v0->position.z,
                     m0->boundingSphere.x, m0->boundingSphere.y, m0->boundingSphere.z,
                     m0->boundingSphere.w, m0->vertexOffset, m0->vertexCount, m0->triangleOffset,
                     m0->triangleCount, m0->padding[0]);
        std::fprintf(stderr, "[bake] all:");
        for (uint32_t i = 0; i < stats_.meshlets; ++i) {
            const auto& mm = m0[i];
            std::fprintf(stderr, " m%u(c=%.0f,%.0f,%.0f,r=%.0f,v=%u,t=%u,p=%u)", i,
                         mm.boundingSphere.x, mm.boundingSphere.y, mm.boundingSphere.z,
                         mm.boundingSphere.w, mm.vertexCount, mm.triangleCount, mm.padding[0]);
        }
        std::fprintf(stderr, "\n");
    }
}
void SceneRenderer::ensureHistory(VkExtent2D extent) {
    if (historyImages_[0] != VK_NULL_HANDLE && historyExtent_.width == extent.width &&
        historyExtent_.height == extent.height) {
        return;
    }
    for (uint32_t i = 0; i < 2; ++i) {
        if (historyViews_[i] != VK_NULL_HANDLE) vkDestroyImageView(device_, historyViews_[i], nullptr);
        if (historyImages_[i] != VK_NULL_HANDLE) vkDestroyImage(device_, historyImages_[i], nullptr);
        if (historyMemories_[i] != VK_NULL_HANDLE) vkFreeMemory(device_, historyMemories_[i], nullptr);
        historyViews_[i] = VK_NULL_HANDLE;
        historyImages_[i] = VK_NULL_HANDLE;
        historyMemories_[i] = VK_NULL_HANDLE;
    }
    historyExtent_ = extent;
    if (extent.width == 0 || extent.height == 0) return;
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_D32_SFLOAT;
    ci.extent = {extent.width, extent.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    for (uint32_t i = 0; i < 2; ++i) {
        if (vkCreateImage(device_, &ci, nullptr, &historyImages_[i]) != VK_SUCCESS) return;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, historyImages_[i], &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =
            findMemoryType(physical_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(device_, &ai, nullptr, &historyMemories_[i]) != VK_SUCCESS) return;
        vkBindImageMemory(device_, historyImages_[i], historyMemories_[i], 0);
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = historyImages_[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_D32_SFLOAT;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        vkCreateImageView(device_, &vi, nullptr, &historyViews_[i]);
    }
    // Steady-state layout is SHADER_READ_ONLY (see history copy pass).
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = graphicsFamily_;
    if (vkCreateCommandPool(device_, &pci, nullptr, &pool) != VK_SUCCESS) return;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo abi{};
    abi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    abi.commandPool = pool;
    abi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    abi.commandBufferCount = 1;
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkAllocateCommandBuffers(device_, &abi, &cb) == VK_SUCCESS &&
        vkCreateFence(device_, &fi, nullptr, &fence) == VK_SUCCESS) {
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier bars[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            bars[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bars[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            bars[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bars[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bars[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bars[i].image = historyImages_[i];
            bars[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            bars[i].subresourceRange.levelCount = 1;
            bars[i].subresourceRange.layerCount = 1;
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 2, bars);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        vkQueueSubmit(graphicsQueue_, 1, &si, fence);
        vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(device_, fence, nullptr);
    }
    vkDestroyCommandPool(device_, pool, nullptr);
}
void SceneRenderer::upload(const GPUScene& scene, const FrameContext& ctx, uint32_t frameSlot) {
    scene_ = &scene;
    ctx_ = ctx;
    activeSlot_ = frameSlot % kFrames;
    stats_ = SceneRenderStats{};
    if (std::getenv("ENGINE_READBACK") && indirect_[activeSlot_].mapped) {
        for (uint32_t s = 0; s < kFrames; ++s) {
            auto* r = static_cast<uint32_t*>(requests_[s].mapped);
            auto* ii = static_cast<uint32_t*>(indirect_[s].mapped);
            std::fprintf(stderr, "[cull] slot=%u idx=%u req1=%u req2=%u req3=%u\n", s, ii[0], r[1],
                         r[2], r[3]);
        }
    }
    auto materialHash = [](const GPUSceneMaterial& m) {
        uint64_t hash = reinterpret_cast<uint64_t>(m.albedoTexture);
        uint32_t bits = 0;
        static_assert(sizeof(float) == sizeof(uint32_t));
        std::memcpy(&bits, &m.metallic, sizeof(float));
        hash ^= static_cast<uint64_t>(bits + 0x9e3779b9 + (hash << 6) + (hash >> 2));
        std::memcpy(&bits, &m.roughness, sizeof(float));
        hash ^= static_cast<uint64_t>(bits + 0x9e3779b9 + (hash << 6) + (hash >> 2));
        return hash;
    };
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const auto& m = scene.materials[i];
        uint64_t hash = materialHash(m);
        auto it = materialSlots_.find(hash);
        if (it == materialSlots_.end()) {
            uint32_t slot = nextMaterial_++;
            uint32_t texSlot = 0;
            if (m.albedoTexture && m.albedoTexture->view != VK_NULL_HANDLE) {
                auto tit = textureSlots_.find(m.albedoTexture);
                if (tit != textureSlots_.end()) {
                    texSlot = tit->second;
                } else {
                    texSlot = bindless_.registerTexture(m.albedoTexture->view);
                    textureSlots_[m.albedoTexture] = texSlot;
                }
            }
            GPUMaterial gpu;
            gpu.albedoTextureID = texSlot;
            gpu.normalTextureID = 1;
            gpu.metallicRoughnessTextureID = 2;
            gpu.samplerID = 0;
            gpu.baseColorFactor = m.baseColor;
            gpu.metallicFactor = m.metallic;
            gpu.roughnessFactor = m.roughness;
            bindless_.updateMaterial(slot, gpu);
            materialSlots_[hash] = slot;
        }
    }
    drawMaterial_.assign(scene.draws.size(), 0);
    for (size_t i = 0; i < scene.draws.size(); ++i) {
        uint32_t matIdx = scene.draws[i].materialID;
        if (matIdx >= scene.materials.size()) continue;
        auto it = materialSlots_.find(materialHash(scene.materials[matIdx]));
        drawMaterial_[i] = it != materialSlots_.end() ? it->second : 0;
    }

    // Cascade shadow matrices (same fitting as the legacy recorder).
    {
        glm::mat4 vp = scene.camera.proj * scene.camera.view;
        glm::mat4 invVP = glm::inverse(vp);
        const float camNear = 0.1f;
        const float camFar = 150.0f;
        const float splitsNorm[4] = {0.05f, 0.15f, 0.35f, 1.0f};
        glm::vec3 dir = glm::normalize(scene.directional.direction);
        auto ndcZ = [&](float d) {
            return ((camFar + camNear) * d - 2.0f * camFar * camNear) / ((camFar - camNear) * d);
        };
        for (uint32_t c = 0; c < 4; ++c) {
            float farDist = camNear + (camFar - camNear) * splitsNorm[c];
            float prevDist = c == 0 ? camNear : camNear + (camFar - camNear) * splitsNorm[c - 1];
            float zn = ndcZ(prevDist), zf = ndcZ(farDist);
            glm::vec3 corners[8];
            uint32_t n = 0;
            for (uint32_t zi = 0; zi < 2; ++zi) {
                float z = zi == 0 ? zn : zf;
                for (float sx : {-1.0f, 1.0f})
                    for (float sy : {-1.0f, 1.0f}) {
                        glm::vec4 wh = invVP * glm::vec4(sx, sy, z, 1.0f);
                        corners[n++] = glm::vec3(wh) / wh.w;
                    }
            }
            glm::vec3 center{0.0f};
            for (auto& corner : corners) center += corner;
            center /= 8.0f;
            glm::mat4 lightView = glm::lookAt(center - dir * 40.0f, center, glm::vec3(0.0f, 1.0f, 0.0f));
            float minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX, minZ = FLT_MAX,
                  maxZ = -FLT_MAX;
            for (auto& corner : corners) {
                glm::vec3 ls = lightView * glm::vec4(corner, 1.0f);
                minX = std::min(minX, ls.x);
                maxX = std::max(maxX, ls.x);
                minY = std::min(minY, ls.y);
                maxY = std::max(maxY, ls.y);
                minZ = std::min(minZ, ls.z);
                maxZ = std::max(maxZ, ls.z);
            }
            float texelX = (maxX - minX) / 1024.0f;
            if (texelX > 0) {
                minX = std::floor(minX / texelX) * texelX;
                maxX = std::ceil(maxX / texelX) * texelX;
            }
            float texelY = (maxY - minY) / 1024.0f;
            if (texelY > 0) {
                minY = std::floor(minY / texelY) * texelY;
                maxY = std::ceil(maxY / texelY) * texelY;
            }
            shadowVPs_[c] = glm::ortho(minX, maxX, minY, maxY, -maxZ - 10.0f, -minZ + 10.0f) * lightView;
        }
    }
    // Static meshlet bake when transforms or topology changed.
    if (staticsDirty(scene)) bakeStatics(scene);
    stats_.instances = static_cast<uint32_t>(scene.draws.size());
    stats_.meshlets = bakedMeshlets_;
    stats_.triangles = bakedTris_;
    stats_.residentPages = bakedPages_ > 0 ? bakedPages_ - 1 : 0;
    stats_.totalPages = bakedPages_;
    for (auto& d : scene.draws) {
        stats_.staticDraws += d.skinned ? 0 : 1;
        stats_.skinnedDraws += d.skinned ? 1 : 0;
    }

    // Classic instance ring (model/color/params for indexed draws + shadow).
    auto* inst = static_cast<ClassicInstance*>(instanceRings_[activeSlot_].mapped);
    uint32_t ninst = 0;
    for (auto& d : scene.draws) {
        if (!d.mesh || d.mesh->empty() || ninst >= kMaxDraws) continue;
        inst[ninst].model = d.model;
        inst[ninst].color = glm::vec4(1.0f);
        const auto& mat = scene.materials[d.materialID < scene.materials.size() ? d.materialID : 0];
        inst[ninst].params = glm::vec4(mat.metallic, mat.roughness, 0.0f, 0.0f);
        ++ninst;
    }
    stats_.draws = ninst;

    // Joints ring.
    auto* joints = static_cast<glm::mat4*>(jointRings_[activeSlot_].mapped);
    size_t njoints = std::min(scene.joints.size(), size_t(kMaxJoints));
    for (size_t i = 0; i < njoints; ++i) joints[i] = scene.joints[i];
    for (size_t i = njoints; i < kMaxJoints; ++i) joints[i] = glm::mat4(1.0f);

    // Camera + deferred frame blocks.
    glm::mat4 vp = scene.camera.proj * scene.camera.view;
    glm::mat4 invVP = glm::inverse(vp);
    auto* cam = static_cast<CameraBlock*>(cameraRings_[activeSlot_].mapped);
    cam->viewProjection = vp;
    cam->cameraPos = glm::vec4(scene.camera.worldPosition, 1.0f);
    cam->lightDir = glm::vec4(glm::normalize(scene.directional.direction), 0.0f);
    cam->lightColor = glm::vec4(scene.directional.color * scene.directional.intensity, 1.0f);
    cam->params = glm::vec4(0.25f, 0.0f, 0.0f, 0.0f);
    cam->lightVP = scene.directional.shadowVP;
    auto* frame = static_cast<DeferredFrameBlock*>(frameUboRings_[activeSlot_].mapped);
    frame->invViewProj = invVP;
    frame->view = scene.camera.view;
    frame->cameraPos = glm::vec4(scene.camera.worldPosition, 1.0f);
    frame->dirLightDir = glm::vec4(glm::normalize(scene.directional.direction), 0.0f);
    frame->dirLightColor = glm::vec4(scene.directional.color, scene.directional.intensity);
    frame->shadowVP = scene.directional.shadowVP;
    frame->shadowParams = glm::vec4(1.0f / 1024.0f, 1.0f, 1024.0f, 0.0f);

    // Lights ring.
    auto* lights = static_cast<GPULight*>(lightRings_[activeSlot_].mapped);
    size_t nlights = std::min(scene.lights.size(), size_t(kMaxLights));
    for (size_t i = 0; i < nlights; ++i) lights[i] = scene.lights[i];
    stats_.lights = static_cast<uint32_t>(nlights);

    // Cull camera blocks + cluster grid dims.
    auto* cullCam = static_cast<ClusterCamBlock*>(cullCamRings_[activeSlot_].mapped);
    cullCam->view = scene.camera.view;
    cullCam->invProj = glm::inverse(scene.camera.proj);
    auto* meshCam = static_cast<MeshletCamBlock*>(meshletCamRings_[activeSlot_].mapped);
    meshCam->view = scene.camera.view;
    meshCam->proj = scene.camera.proj;
    extractFrustum(vp, meshCam->frustum);
    uint32_t gx = std::min((ctx.renderExtent.width + 63) / 64, kGridXCap);
    uint32_t gy = std::min((ctx.renderExtent.height + 63) / 64, kGridYCap);
    clusterCount_ = std::max(1u, gx * gy * kClusterZ);

    // Reset per-frame GPU counters (fill + barrier recorded in cull passes).
    static_cast<uint32_t*>(clusterIndex_[activeSlot_].mapped)[0] = 0;
    static_cast<uint32_t*>(indirect_[activeSlot_].mapped)[0] = 0;
    static_cast<uint32_t*>(indirect_[activeSlot_].mapped)[1] = 1;
    static_cast<uint32_t*>(requests_[activeSlot_].mapped)[0] = 0;
    std::memset(static_cast<uint32_t*>(requests_[activeSlot_].mapped) + 900, 0, 125 * sizeof(uint32_t));

    ensureHistory(ctx.renderExtent);
    writeFrameDescriptors(activeSlot_);
}

bool SceneRenderer::createDescriptorPool() {
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 16},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 24},
    };
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 64;
    pi.poolSizeCount = 6;
    pi.pPoolSizes = sizes;
    return vkCreateDescriptorPool(device_, &pi, nullptr, &pool_) == VK_SUCCESS;
}

static VkDescriptorSetLayout makeLayout(VkDevice device, const VkDescriptorSetLayoutBinding* bindings,
                                        uint32_t count) {
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = count;
    li.pBindings = bindings;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(device, &li, nullptr, &layout);
    return layout;
}

static VkDescriptorSet allocSet(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(device, &ai, &set);
    return set;
}

bool SceneRenderer::createLayouts() {
    VkDescriptorSetLayoutBinding ubo{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                         VK_SHADER_STAGE_COMPUTE_BIT,
                                     nullptr};
    VkDescriptorSetLayoutBinding jointBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                              VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    cameraSetLayout_ = makeLayout(device_, &ubo, 1);
    jointSetLayout_ = makeLayout(device_, &jointBinding, 1);
    frameUboLayout_ = makeLayout(device_, &ubo, 1);
    if (!cameraSetLayout_ || !jointSetLayout_ || !frameUboLayout_) return false;

    VkDescriptorSetLayoutBinding cullBindings[4]{};
    cullBindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    cullBindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    cullBindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    cullBindings[3] = {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    cullLayout_ = makeLayout(device_, cullBindings, 4);

    VkDescriptorSetLayoutBinding hizBindings[2]{};
    hizBindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    hizBindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    hizLayout_ = makeLayout(device_, hizBindings, 2);

    VkDescriptorSetLayoutBinding mcBindings[10]{};
    for (uint32_t i = 0; i < 9; ++i) {
        mcBindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    mcBindings[6] = {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    mcBindings[7] = {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    mcBindings[8] = {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutBinding mcUbo{9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                       nullptr};
    mcBindings[9] = mcUbo;
    meshletCullLayout_ = makeLayout(device_, mcBindings, 10);

    VkDescriptorSetLayoutBinding vtxBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                            VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    meshletGfxLayout_ = makeLayout(device_, &vtxBinding, 1);

    VkDescriptorSetLayoutBinding gbufBindings[5]{};
    for (uint32_t i = 0; i < 5; ++i) {
        gbufBindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                           nullptr};
    }
    deferredGbufferLayout_ = makeLayout(device_, gbufBindings, 5);

    VkDescriptorSetLayoutBinding lightBindings[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        lightBindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    deferredLightLayout_ = makeLayout(device_, lightBindings, 3);

    VkDescriptorSetLayoutBinding frameBindings[5]{};
    frameBindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    frameBindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                        nullptr};
    for (uint32_t i = 2; i < 5; ++i) {
        frameBindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                            nullptr};
    }
    deferredFrameLayout_ = makeLayout(device_, frameBindings, 5);

    VkDescriptorSetLayoutBinding tmBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                           VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

    tonemapLayout_ = makeLayout(device_, &tmBinding, 1);
    emptyLayout_ = makeLayout(device_, nullptr, 0);
    return cullLayout_ && hizLayout_ && meshletCullLayout_ && meshletGfxLayout_ && deferredGbufferLayout_ &&
           deferredLightLayout_ && deferredFrameLayout_ && tonemapLayout_ && emptyLayout_;
}
bool SceneRenderer::createStaticDescriptors() {
    for (uint32_t i = 0; i < kFrames; ++i) {
        cameraSets_[i] = allocSet(device_, pool_, cameraSetLayout_);
        jointSets_[i] = allocSet(device_, pool_, jointSetLayout_);
        frameUboSets_[i] = allocSet(device_, pool_, frameUboLayout_);
        cullSets_[i] = allocSet(device_, pool_, cullLayout_);
        meshletCullSets_[i] = allocSet(device_, pool_, meshletCullLayout_);
        if (!cameraSets_[i] || !jointSets_[i] || !frameUboSets_[i] || !cullSets_[i] ||
            !meshletCullSets_[i]) {
            return false;
        }
    }
    for (uint32_t i = 0; i < kFrames; ++i) {
        meshletGfxSets_[i] = allocSet(device_, pool_, meshletGfxLayout_);
        deferredGbufferSets_[i] = allocSet(device_, pool_, deferredGbufferLayout_);
        deferredLightSets_[i] = allocSet(device_, pool_, deferredLightLayout_);
        deferredFrameSets_[i] = allocSet(device_, pool_, deferredFrameLayout_);
        tonemapSets_[i] = allocSet(device_, pool_, tonemapLayout_);
        if (!meshletGfxSets_[i] || !deferredGbufferSets_[i] || !deferredLightSets_[i] ||
            !deferredFrameSets_[i] || !tonemapSets_[i]) {
            return false;
        }
        for (uint32_t m = 0; m < 12; ++m) {
            hizSets_[i][m] = allocSet(device_, pool_, hizLayout_);
            if (hizSets_[i][m] == VK_NULL_HANDLE) return false;
        }
    }
    return true;
}

void SceneRenderer::writeFrameDescriptors(uint32_t frameSlot) {
    auto uboWrite = [&](VkDescriptorSet set, uint32_t binding, VkBuffer buffer, VkDeviceSize size) {
        VkDescriptorBufferInfo info{};
        info.buffer = buffer;
        info.range = size;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.descriptorCount = 1;
        w.pBufferInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    };
    auto ssboWrite = [&](VkDescriptorSet set, uint32_t binding, VkBuffer buffer, VkDeviceSize size) {
        VkDescriptorBufferInfo info{};
        info.buffer = buffer;
        info.range = size;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.descriptorCount = 1;
        w.pBufferInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    };
    uboWrite(cameraSets_[frameSlot], 0, cameraRings_[frameSlot].buffer, sizeof(CameraBlock));
    ssboWrite(jointSets_[frameSlot], 0, jointRings_[frameSlot].buffer, kMaxJoints * sizeof(glm::mat4));
    uboWrite(frameUboSets_[frameSlot], 0, frameUboRings_[frameSlot].buffer, sizeof(DeferredFrameBlock));
    uboWrite(cullSets_[frameSlot], 3, cullCamRings_[frameSlot].buffer, sizeof(ClusterCamBlock));
    uboWrite(meshletCullSets_[frameSlot], 9, meshletCamRings_[frameSlot].buffer, sizeof(MeshletCamBlock));
    ssboWrite(cullSets_[frameSlot], 0, lightRings_[frameSlot].buffer, kMaxLights * sizeof(GPULight));
    ssboWrite(cullSets_[frameSlot], 1, clusterGrid_[activeSlot_].buffer, VK_WHOLE_SIZE);
    ssboWrite(cullSets_[frameSlot], 2, clusterIndex_[activeSlot_].buffer, VK_WHOLE_SIZE);
    ssboWrite(meshletCullSets_[frameSlot], 0, meshletPool_.buffer, VK_WHOLE_SIZE);
    ssboWrite(meshletCullSets_[frameSlot], 1, instanceRings_[frameSlot].buffer, VK_WHOLE_SIZE);
    ssboWrite(meshletCullSets_[frameSlot], 2, uniquePool_.buffer, VK_WHOLE_SIZE);
    ssboWrite(meshletCullSets_[frameSlot], 3, triPool_.buffer, VK_WHOLE_SIZE);
    ssboWrite(meshletCullSets_[frameSlot], 4, compacted_[activeSlot_].buffer, VK_WHOLE_SIZE);
    ssboWrite(meshletCullSets_[frameSlot], 5, indirect_[activeSlot_].buffer, VK_WHOLE_SIZE);
    ssboWrite(meshletCullSets_[frameSlot], 7, residency_[activeSlot_].buffer, VK_WHOLE_SIZE);
    ssboWrite(meshletCullSets_[frameSlot], 8, requests_[activeSlot_].buffer, VK_WHOLE_SIZE);
    // Deferred light set references the same cluster buffers.
    ssboWrite(deferredLightSets_[activeSlot_], 0, lightRings_[frameSlot].buffer, kMaxLights * sizeof(GPULight));
    ssboWrite(deferredLightSets_[activeSlot_], 1, clusterGrid_[activeSlot_].buffer, VK_WHOLE_SIZE);
    ssboWrite(deferredLightSets_[activeSlot_], 2, clusterIndex_[activeSlot_].buffer, VK_WHOLE_SIZE);
    // Vertex pool grows on rebake; rebind every frame (single write).
    if (vertexPool_.buffer != VK_NULL_HANDLE) {
        ssboWrite(meshletGfxSets_[activeSlot_], 0, vertexPool_.buffer, VK_WHOLE_SIZE);
    }
}

static VkPipelineLayout makePipeLayout(VkDevice device, const VkDescriptorSetLayout* sets, uint32_t setCount,
                                       const VkPushConstantRange* push, uint32_t pushCount) {
    VkPipelineLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    li.setLayoutCount = setCount;
    li.pSetLayouts = sets;
    li.pushConstantRangeCount = pushCount;
    li.pPushConstantRanges = push;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(device, &li, nullptr, &layout);
    return layout;
}

static VkPipeline makeComputePipeline(VkDevice device, VkPipelineLayout layout, VkShaderModule mod) {
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = mod;
    stage.pName = "main";
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage = stage;
    ci.layout = layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe);
    return pipe;
}

bool SceneRenderer::createPipelines(VkFormat swapchainFormat) {
    VkPushConstantRange cullPush{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
    {
        const VkDescriptorSetLayout sets[] = {cullLayout_};
        cullPipeLayout_ = makePipeLayout(device_, sets, 1, &cullPush, 1);
        VkShaderModule mod = loadShader("cluster_cull.comp.spv");
        cullPipeline_ = makeComputePipeline(device_, cullPipeLayout_, mod);
        vkDestroyShaderModule(device_, mod, nullptr);
    }
    {
        const VkDescriptorSetLayout sets[] = {hizLayout_};
        VkPushConstantRange hizPush{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
        hizPipeLayout_ = makePipeLayout(device_, sets, 1, &hizPush, 1);
        VkShaderModule mod = loadShader("hiz_build.comp.spv");
        hizPipeline_ = makeComputePipeline(device_, hizPipeLayout_, mod);
        vkDestroyShaderModule(device_, mod, nullptr);
    }
    VkPushConstantRange meshletPush{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 };
    {
        const VkDescriptorSetLayout sets[] = {meshletCullLayout_};
        meshletCullPipeLayout_ = makePipeLayout(device_, sets, 1, &meshletPush, 1);
        VkShaderModule mod = loadShader("meshlet_cull.comp.spv");
        meshletCullPipeline_ = makeComputePipeline(device_, meshletCullPipeLayout_, mod);
        vkDestroyShaderModule(device_, mod, nullptr);
    }
    if (!cullPipeline_ || !hizPipeline_ || !meshletCullPipeline_) return false;

    // Graphics pipelines (dynamic rendering, no render pass).
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    auto graphicsPipe = [&](const VkDescriptorSetLayout* sets, uint32_t setCount,
                            const VkPushConstantRange* push, uint32_t pushCount, VkShaderModule vert,
                            VkShaderModule frag, const VkFormat* colorFormats, uint32_t colorCount,
                            VkFormat depthFormat, VkPipelineLayout& outLayout, VkPipeline& outPipe,
                            const VkVertexInputBindingDescription* bindings = nullptr,
                            uint32_t bindingCount = 0,
                            const VkVertexInputAttributeDescription* attrs = nullptr,
                            uint32_t attrCount = 0, bool depthWrite = true,
                            VkCompareOp depthOp = VK_COMPARE_OP_LESS_OR_EQUAL,
                            VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT,
                            VkFrontFace frontFace = VK_FRONT_FACE_CLOCKWISE) {
        outLayout = makePipeLayout(device_, sets, setCount, push, pushCount);
        rendering.colorAttachmentCount = colorCount;
        rendering.pColorAttachmentFormats = colorFormats;
        rendering.depthAttachmentFormat = depthFormat;
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = bindingCount;
        vi.pVertexBindingDescriptions = bindings;
        vi.vertexAttributeDescriptionCount = attrCount;
        vi.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{0, 0, 1, 1, 0, 1};
        VkRect2D sc{{0, 0}, {1, 1}};
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.pViewports = &vp;
        vps.scissorCount = 1;
        vps.pScissors = &sc;
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = cullMode;
        rs.frontFace = frontFace; // Vulkan Y-flip inverts screen winding vs legacy GL convention
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = depthFormat != VK_FORMAT_UNDEFINED ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = depthOp;
        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dys{};
        dys.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dys.dynamicStateCount = 2;
        dys.pDynamicStates = dyn;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cbs{};
        cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbs.attachmentCount = colorCount;
        std::vector<VkPipelineColorBlendAttachmentState> blends(colorCount, blend);
        cbs.pAttachments = blends.data();
        VkGraphicsPipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.pNext = &rendering;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vps;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cbs;
        ci.pDynamicState = &dys;
        ci.layout = outLayout;
        return vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &outPipe) ==
               VK_SUCCESS;
    };

    // Classic GBuffer (legacy vertex layout + material push).
    VkVertexInputBindingDescription classicBindings[2]{};
    classicBindings[0] = {0, sizeof(::engine::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    classicBindings[1] = {1, sizeof(ClassicInstance), VK_VERTEX_INPUT_RATE_INSTANCE};
    VkVertexInputAttributeDescription classicAttrs[11]{};
    classicAttrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(::engine::Vertex, position)};
    classicAttrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(::engine::Vertex, normal)};
    classicAttrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(::engine::Vertex, uv)};
    classicAttrs[3] = {9, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(::engine::Vertex, jointIndices)};
    classicAttrs[4] = {10, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(::engine::Vertex, jointWeights)};
    for (int i = 0; i < 4; ++i) {
        classicAttrs[5 + i] = {static_cast<uint32_t>(3 + i), 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                               offsetof(ClassicInstance, model) + i * sizeof(glm::vec4)};
    }
    classicAttrs[9] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ClassicInstance, color)};
    classicAttrs[10] = {8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ClassicInstance, params)};
    const VkFormat gbufferFormats[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R16G16B16A16_SFLOAT,
                                       VK_FORMAT_R8G8B8A8_UNORM};
    VkPushConstantRange classicPush{ VK_SHADER_STAGE_VERTEX_BIT, 0, 4 };
    bool ok = true;
    VkShaderModule gbVert = loadShader("gbuffer_bindless.vert.spv");
    VkShaderModule gbFrag = loadShader("gbuffer_bindless.frag.spv");
    {
        const VkDescriptorSetLayout sets[] = {cameraSetLayout_, bindless_.getDescriptorSetLayout(),
                                              emptyLayout_, emptyLayout_, jointSetLayout_};
        ok &= graphicsPipe(sets, 5, &classicPush, 1, gbVert, gbFrag, gbufferFormats, 3,
                            VK_FORMAT_D32_SFLOAT, gbufferPipeLayout_, gbufferClassicPipeline_,
                            classicBindings, 2, classicAttrs, 11, true);
    }
    VkShaderModule gmVert = loadShader("gbuffer_meshlet.vert.spv");
    VkPushConstantRange meshletVertPush{ VK_SHADER_STAGE_VERTEX_BIT, 0, 64 };
    {
        const VkDescriptorSetLayout sets[] = {meshletGfxLayout_, bindless_.getDescriptorSetLayout()};
        ok &= graphicsPipe(sets, 2, &meshletVertPush, 1, gmVert, gbFrag, gbufferFormats, 3,
                            VK_FORMAT_D32_SFLOAT, gbufferMeshletPipeLayout_, gbufferMeshletPipeline_,
                            nullptr, 0, nullptr, 0, true);
    }
    vkDestroyShaderModule(device_, gbVert, nullptr);
    vkDestroyShaderModule(device_, gbFrag, nullptr);
    vkDestroyShaderModule(device_, gmVert, nullptr);
    VkPushConstantRange deferredPush{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48 };
    VkShaderModule dqVert = loadShader("deferred_lighting.vert.spv");
    VkShaderModule dqFrag = loadShader("deferred_lighting.frag.spv");
    {
        const VkFormat hdrFormat[] = {VK_FORMAT_R16G16B16A16_SFLOAT};
        const VkDescriptorSetLayout sets[] = {deferredGbufferLayout_, deferredLightLayout_,
                                              deferredFrameLayout_};
        ok &= graphicsPipe(sets, 3, &deferredPush, 1, dqVert, dqFrag, hdrFormat, 1,
                            VK_FORMAT_UNDEFINED, deferredPipeLayout_, deferredPipeline_);
    }
    vkDestroyShaderModule(device_, dqVert, nullptr);
    vkDestroyShaderModule(device_, dqFrag, nullptr);
    VkPushConstantRange tonemapPush{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 };
    VkShaderModule tmVert = loadShader("tonemap.vert.spv");
    VkShaderModule tmFrag = loadShader("tonemap.frag.spv");
    {
        const VkFormat swapFormat[] = {swapchainFormat};
        const VkDescriptorSetLayout sets[] = {tonemapLayout_};
        ok &= graphicsPipe(sets, 1, &tonemapPush, 1, tmVert, tmFrag, swapFormat, 1,
                            VK_FORMAT_UNDEFINED, tonemapPipeLayout_, tonemapPipeline_);
    }
    vkDestroyShaderModule(device_, tmVert, nullptr);
    vkDestroyShaderModule(device_, tmFrag, nullptr);
    VkPushConstantRange skyPush{ VK_SHADER_STAGE_VERTEX_BIT, 0, 80 };
    VkShaderModule sbVert = loadShader("skybox.vert.spv");
    VkShaderModule sbFrag = loadShader("skybox.frag.spv");
    {
        const VkFormat hdrFormat[] = {VK_FORMAT_R16G16B16A16_SFLOAT};
        const VkDescriptorSetLayout sets[] = {emptyLayout_, emptyLayout_, emptyLayout_,
                                              legacyDevice_->set3Layout()};
        VkVertexInputBindingDescription skyBinding{0, sizeof(glm::vec3), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription skyAttr{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        ok &= graphicsPipe(sets, 4, &skyPush, 1, sbVert, sbFrag, hdrFormat, 1, VK_FORMAT_D32_SFLOAT,
                            skyboxPipeLayout_, skyboxPipeline_, &skyBinding, 1, &skyAttr, 1, false,
                            VK_COMPARE_OP_LESS_OR_EQUAL, VK_CULL_MODE_NONE);
    }
    vkDestroyShaderModule(device_, sbVert, nullptr);
    vkDestroyShaderModule(device_, sbFrag, nullptr);
    return ok;
}

bool SceneRenderer::createSamplers() {
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = VK_FILTER_LINEAR;
    ci.minFilter = VK_FILTER_LINEAR;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.minLod = 0.0f;
    ci.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device_, &ci, nullptr, &hizSampler_) != VK_SUCCESS) return false;
    if (vkCreateSampler(device_, &ci, nullptr, &gbufferSampler_) != VK_SUCCESS) return false;
    return true;
}

void SceneRenderer::destroyPipelines() {
    if (device_ == VK_NULL_HANDLE) return;
    const VkPipeline pipes[] = {cullPipeline_,         hizPipeline_,     meshletCullPipeline_,
                                gbufferClassicPipeline_, gbufferMeshletPipeline_, deferredPipeline_,
                                tonemapPipeline_,      skyboxPipeline_};
    for (auto p : pipes)
        if (p != VK_NULL_HANDLE) vkDestroyPipeline(device_, p, nullptr);
    cullPipeline_ = hizPipeline_ = meshletCullPipeline_ = gbufferClassicPipeline_ =
        gbufferMeshletPipeline_ = deferredPipeline_ = tonemapPipeline_ = skyboxPipeline_ =
            VK_NULL_HANDLE;
    const VkPipelineLayout layouts[] = {
        cullPipeLayout_, hizPipeLayout_, meshletCullPipeLayout_, gbufferPipeLayout_,
        gbufferMeshletPipeLayout_, deferredPipeLayout_, tonemapPipeLayout_, skyboxPipeLayout_};
    for (auto l : layouts)
        if (l != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, l, nullptr);
    cullPipeLayout_ = hizPipeLayout_ = meshletCullPipeLayout_ = gbufferPipeLayout_ =
        gbufferMeshletPipeLayout_ = deferredPipeLayout_ = tonemapPipeLayout_ = skyboxPipeLayout_ =
            VK_NULL_HANDLE;
}

void SceneRenderer::destroyDescriptors() {
    if (device_ == VK_NULL_HANDLE) return;
    const VkDescriptorSetLayout layouts[] = {
        cameraSetLayout_, jointSetLayout_, frameUboLayout_, emptyLayout_, cullLayout_, hizLayout_,
        meshletCullLayout_, meshletGfxLayout_, deferredGbufferLayout_, deferredLightLayout_,
        deferredFrameLayout_, tonemapLayout_};
    for (auto l : layouts)
        if (l != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, l, nullptr);
    cameraSetLayout_ = jointSetLayout_ = frameUboLayout_ = emptyLayout_ = cullLayout_ = hizLayout_ =
        meshletCullLayout_ = meshletGfxLayout_ = deferredGbufferLayout_ = deferredLightLayout_ =
            deferredFrameLayout_ = tonemapLayout_ = VK_NULL_HANDLE;
    if (hizSampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, hizSampler_, nullptr);
    if (gbufferSampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, gbufferSampler_, nullptr);
    hizSampler_ = gbufferSampler_ = VK_NULL_HANDLE;
    for (auto& row : hizSets_)
        for (auto& s : row) s = VK_NULL_HANDLE;
}


void SceneRenderer::beginTarget(VkCommandBuffer cb, std::initializer_list<VkRenderingAttachmentInfo> colors,
                                const VkRenderingAttachmentInfo* depth, VkExtent2D extent) {
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea.offset = {0, 0};
    info.renderArea.extent = extent;
    info.layerCount = 1;
    info.colorAttachmentCount = static_cast<uint32_t>(colors.size());
    info.pColorAttachments = colors.size() ? colors.begin() : nullptr;
    info.pDepthAttachment = depth;
    VkViewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.maxDepth = 1.0f;
    VkRect2D sc{{0, 0}, extent};
    vkCmdBeginRendering(cb, &info);
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

void SceneRenderer::recordClusterCull(VkCommandBuffer cb, const FrameContext& ctx) {
    (void)ctx;
    if (clusterCount_ == 0) return;
    vkCmdFillBuffer(cb, clusterIndex_[activeSlot_].buffer, 0, sizeof(uint32_t), 0);
    VkBufferMemoryBarrier fillBar{};
    fillBar.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    fillBar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fillBar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    fillBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fillBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fillBar.buffer = clusterIndex_[activeSlot_].buffer;
    fillBar.offset = 0;
    fillBar.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 1, &fillBar, 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeLayout_, 0, 1,
                            &cullSets_[activeSlot_], 0, nullptr);
    struct Push {
        uint32_t gx, gy, gz, numLights;
        float w, h, zn, zf;
    } push{};
    uint32_t gx = std::min((ctx_.renderExtent.width + 63) / 64, kGridXCap);
    uint32_t gy = std::min((ctx_.renderExtent.height + 63) / 64, kGridYCap);
    push.gx = gx;
    push.gy = gy;
    push.gz = kClusterZ;
    push.numLights = stats_.lights;
    push.w = static_cast<float>(ctx_.renderExtent.width);
    push.h = static_cast<float>(ctx_.renderExtent.height);
    push.zn = scene_ ? scene_->camera.zNear : 0.1f;
    push.zf = scene_ ? scene_->camera.zFar : 1000.0f;
    vkCmdPushConstants(cb, cullPipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cb, clusterCount_, 1, 1);
}

void SceneRenderer::recordHiZBuild(VkCommandBuffer cb, const FrameContext& ctx) {
    (void)ctx;
    if (refs_.hizMips < 1 || refs_.hizExtent.width == 0) return;
    auto& res = activeGraph_->resources()[refs_.hiz.id];
    // Per-mip views (slot-gated: destroyed on slot reuse after fence wait).
    for (auto v : hizViews_[activeSlot_]) vkDestroyImageView(device_, v, nullptr);
    hizViews_[activeSlot_].clear();
    for (uint32_t m = 0; m < refs_.hizMips; ++m) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = res.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R32_SFLOAT;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.baseMipLevel = m;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(device_, &vi, nullptr, &view);
        hizViews_[activeSlot_].push_back(view);
    }
    auto& gbuf = activeGraph_->resources()[refs_.gbuffer.depth.id];
    (void)gbuf;
    const VkImageView historyReadView = activeGraph_->resources()[refs_.historyRead.id].view;
    for (uint32_t m = 0; m < refs_.hizMips; ++m) {
        VkDescriptorImageInfo parentInfo{};
        parentInfo.sampler = hizSampler_;
        parentInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (m == 0) {
            parentInfo.imageView = historyReadView;
        } else {
            parentInfo.imageView = hizViews_[activeSlot_][m - 1];
        }
        VkDescriptorImageInfo childInfo{};
        childInfo.imageView = hizViews_[activeSlot_][m];
        childInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = hizSets_[activeSlot_][m];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &parentInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = hizSets_[activeSlot_][m];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &childInfo;
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hizPipeline_);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hizPipeLayout_, 0, 1, &hizSets_[activeSlot_][m],
                                0, nullptr);
        const VkExtent2D childExtent = HiZPyramid::mipExtent(refs_.hizExtent, m);
        struct Push {
            float w, h;
        } push{static_cast<float>(childExtent.width), static_cast<float>(childExtent.height)};
        vkCmdPushConstants(cb, hizPipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        uint32_t dx = (static_cast<uint32_t>(push.w) + 15) / 16;
        uint32_t dy = (static_cast<uint32_t>(push.h) + 15) / 16;
        vkCmdDispatch(cb, std::max(1u, dx), std::max(1u, dy), 1);
        {
            VkImageMemoryBarrier bar{};
            bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image = res.image;
            bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bar.subresourceRange.baseMipLevel = m;
            bar.subresourceRange.levelCount = 1;
            bar.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &bar);
        }
    }
    // Manual per-mip barriers above leave every mip sampled; publish it so
    // aliased reuse next frame derives the correct entry barrier.
    auto& hizRes = activeGraph_->resources()[refs_.hiz.id];
    if (hizRes.physicalBinding) {
        hizRes.physicalBinding->currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}

void SceneRenderer::recordShadow(VkCommandBuffer cb, const FrameContext& ctx) {
    (void)ctx;
    if (!scene_ || !shadowPass_) return;
    VkBuffer instances = instanceRings_[activeSlot_].buffer;
    uint32_t instSlot = 0;
    for (uint32_t c = 0; c < ::engine::VulkanShadowPass::kCascadeCount; ++c) {
        shadowPass_->begin(cb, c);
        uint32_t slot = 0;
        for (auto& d : scene_->draws) {
            bool valid = d.mesh && !d.mesh->empty() && slot < kMaxDraws;
            uint32_t mySlot = slot++;
            if (!valid) continue;
            MeshBuffers& mb = meshBuffers(d.mesh);
            shadowPass_->drawBatchAt(cb, mb.verts.buffer, mb.indices.buffer, mb.indexCount, 1,
                                     mySlot, shadowVPs_[c], instances);
        }
        shadowPass_->end(cb);
        (void)instSlot;
    }
    stats_.draws += static_cast<uint32_t>(scene_->draws.size()) *
                    ::engine::VulkanShadowPass::kCascadeCount;
}
void SceneRenderer::recordMeshletCull(VkCommandBuffer cb, const FrameContext& ctx) {
    (void)ctx;
    if (stats_.meshlets == 0) return;
    vkCmdFillBuffer(cb, requests_[activeSlot_].buffer, 0, sizeof(uint32_t), 0);
    VkBufferMemoryBarrier fillBar{};
    fillBar.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    fillBar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fillBar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    fillBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fillBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fillBar.buffer = requests_[activeSlot_].buffer;
    fillBar.offset = 0;
    fillBar.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, nullptr, 1, &fillBar, 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, meshletCullPipeline_);
    // HiZ view resolves per frame (transient); (re)write binding 6 here.
    {
        VkDescriptorImageInfo hizInfo{};
        hizInfo.sampler = hizSampler_;
        hizInfo.imageView = activeGraph_->resources()[refs_.hiz.id].view;
        hizInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = meshletCullSets_[activeSlot_];
        w.dstBinding = 6;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo = &hizInfo;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, meshletCullPipeLayout_, 0, 1,
                            &meshletCullSets_[activeSlot_], 0, nullptr);
    struct Push {
        glm::vec3 camPos;
        float zNear;
        glm::vec2 hizExtent;
        uint32_t maxMip;
        uint32_t total;
    } push{};
    push.camPos = scene_ ? scene_->camera.worldPosition : glm::vec3(0.0f);
    push.zNear = scene_ ? scene_->camera.zNear : 0.1f;
    push.hizExtent = glm::vec2(refs_.hizExtent.width, refs_.hizExtent.height);
    push.maxMip = refs_.hizMips > 0 ? refs_.hizMips - 1 : 0;
    push.total = stats_.meshlets;
    vkCmdPushConstants(cb, meshletCullPipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
                       &push);
    vkCmdDispatch(cb, (stats_.meshlets + 63) / 64, 1, 1);
}

void SceneRenderer::recordGBuffer(VkCommandBuffer cb, const FrameContext& ctx) {
    if (!scene_) return;
    auto& g = activeGraph_->resources();
    const auto& albedo = g[refs_.gbuffer.albedoAO.id];
    const auto& normal = g[refs_.gbuffer.normalRoughness.id];
    const auto& metallic = g[refs_.gbuffer.metallicFlags.id];
    const auto& depth = g[refs_.gbuffer.depth.id];
    VkRenderingAttachmentInfo colors[3]{};
    const VkImageView views[3] = {albedo.view, normal.view, metallic.view};
    for (int i = 0; i < 3; ++i) {
        colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colors[i].imageView = views[i];
        colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depthAtt{};
    depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAtt.imageView = depth.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.clearValue.depthStencil = {1.0f, 0};
    beginTarget(cb, {colors[0], colors[1], colors[2]}, &depthAtt, ctx.renderExtent);

    // Meshlet-consolidated statics (single indirect draw, world-baked).
    if (stats_.meshlets > 0 && indirect_[activeSlot_].buffer != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gbufferMeshletPipeline_);
        VkDescriptorSet sets[] = {meshletGfxSets_[activeSlot_], bindless_.getDescriptorSet()};
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gbufferMeshletPipeLayout_, 0, 2,
                                sets, 0, nullptr);
        glm::mat4 vp = scene_->camera.proj * scene_->camera.view;
        vkCmdPushConstants(cb, gbufferMeshletPipeLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(vp),
                           &vp);
        vkCmdBindIndexBuffer(cb, compacted_[activeSlot_].buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(cb, indirect_[activeSlot_].buffer, 0, 1,
                                 sizeof(VkDrawIndexedIndirectCommand));
        ++stats_.draws;
    }
    // Classic indexed draws for skinned instances (statics go through meshlets).
    uint32_t instSlot = 0;
    for (size_t di = 0; di < scene_->draws.size(); ++di) {
        const auto& d = scene_->draws[di];
        bool valid = d.mesh && !d.mesh->empty() && instSlot < kMaxDraws;
        ++instSlot;
        if (!valid || !d.skinned) continue;
        MeshBuffers& mb = meshBuffers(d.mesh);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gbufferClassicPipeline_);
        VkDescriptorSet sets[] = {cameraSets_[activeSlot_], bindless_.getDescriptorSet(),
                                  jointSets_[activeSlot_]};
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gbufferPipeLayout_, 0, 2, sets,
                                0, nullptr);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gbufferPipeLayout_, 4, 1,
                                &sets[2], 0, nullptr);
        VkBuffer vbs[] = {mb.verts.buffer, instanceRings_[activeSlot_].buffer};
        VkDeviceSize offs[] = {0, (di < kMaxDraws ? di : 0) * sizeof(ClassicInstance)};
        vkCmdBindVertexBuffers(cb, 0, 2, vbs, offs);
        vkCmdBindIndexBuffer(cb, mb.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
        uint32_t matID = di < drawMaterial_.size() ? drawMaterial_[di] : 0;
        vkCmdPushConstants(cb, gbufferPipeLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(matID),
                           &matID);
        vkCmdDrawIndexed(cb, mb.indexCount, 1, 0, 0, 0);
        ++stats_.draws;
        stats_.triangles += mb.indexCount / 3;
    }
    vkCmdEndRendering(cb);
}

void SceneRenderer::recordDeferred(VkCommandBuffer cb, const FrameContext& ctx) {
    (void)ctx;
    auto& g = activeGraph_->resources();
    const auto& albedo = g[refs_.gbuffer.albedoAO.id];
    const auto& normal = g[refs_.gbuffer.normalRoughness.id];
    const auto& metallic = g[refs_.gbuffer.metallicFlags.id];
    const auto& depth = g[refs_.gbuffer.depth.id];
    const auto& hdr = g[refs_.hdr.id];
    const VkImageView views[4] = {albedo.view, normal.view, metallic.view, depth.view};
    const VkImageView hizView = g[refs_.hiz.id].view;
    for (uint32_t i = 0; i < 4; ++i) {
        VkDescriptorImageInfo info{};
        info.sampler = gbufferSampler_;
        info.imageView = views[i];
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = deferredGbufferSets_[activeSlot_];
        w.dstBinding = i;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }
    {
        VkDescriptorImageInfo hizInfo{};
        hizInfo.sampler = gbufferSampler_;
        hizInfo.imageView = hizView;
        hizInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = deferredGbufferSets_[activeSlot_];
        w.dstBinding = 4;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo = &hizInfo;
        vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }
    // Frame set: UBO already bound per slot at writeFrameDescriptors time? No:
    // rebind slot UBO + shadow + IBL here (views persistent except UBO slot).
    {
        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = frameUboRings_[activeSlot_].buffer;
        uboInfo.range = sizeof(DeferredFrameBlock);
        VkDescriptorImageInfo shadowInfo{};
        shadowInfo.sampler = shadowPass_->sampler();
        shadowInfo.imageView = shadowPass_->view(0);
        shadowInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo iblInfo[3]{};
        iblInfo[0].sampler = environment_->irradianceSampler();
        iblInfo[0].imageView = environment_->irradianceView();
        iblInfo[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        iblInfo[1].sampler = environment_->prefilteredSampler();
        iblInfo[1].imageView = environment_->prefilteredView();
        iblInfo[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        iblInfo[2].sampler = environment_->brdfSampler();
        iblInfo[2].imageView = environment_->brdfView();
        iblInfo[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet writes[5]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = deferredFrameSets_[activeSlot_];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &uboInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = deferredFrameSets_[activeSlot_];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &shadowInfo;
        for (uint32_t i = 0; i < 3; ++i) {
            writes[2 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2 + i].dstSet = deferredFrameSets_[activeSlot_];
            writes[2 + i].dstBinding = 2 + i;
            writes[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2 + i].descriptorCount = 1;
            writes[2 + i].pImageInfo = &iblInfo[i];
        }
        vkUpdateDescriptorSets(device_, 5, writes, 0, nullptr);
    }
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = hdr.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    beginTarget(cb, {color}, nullptr, ctx_.renderExtent);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, deferredPipeline_);
    VkDescriptorSet sets[] = {deferredGbufferSets_[activeSlot_], deferredLightSets_[activeSlot_], deferredFrameSets_[activeSlot_]};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, deferredPipeLayout_, 0, 3, sets, 0,
                            nullptr);
    struct Push {
        uint32_t gx, gy, gz, pad0;
        float w, h, zn, zf;
        float exposure;
        int32_t debugView;
        uint32_t pad1[2];
    } push{};
    push.gx = std::min((ctx_.renderExtent.width + 63) / 64, kGridXCap);
    push.gy = std::min((ctx_.renderExtent.height + 63) / 64, kGridYCap);
    push.gz = kClusterZ;
    push.w = static_cast<float>(ctx_.renderExtent.width);
    push.h = static_cast<float>(ctx_.renderExtent.height);
    push.zn = scene_ ? scene_->camera.zNear : 0.1f;
    push.zf = scene_ ? scene_->camera.zFar : 1000.0f;
    push.exposure = exposure_;
    push.debugView = debugView_;
    vkCmdPushConstants(cb, deferredPipeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

void SceneRenderer::recordForward(VkCommandBuffer cb, const FrameContext& ctx) {
    (void)ctx;
    auto& g = activeGraph_->resources();
    const auto& hdr = g[refs_.hdr.id];
    const auto& depth = g[refs_.gbuffer.depth.id];
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = hdr.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depthAtt{};
    depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAtt.imageView = depth.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    beginTarget(cb, {color}, &depthAtt, ctx_.renderExtent);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, skyboxPipeline_);
    VkDescriptorSet envSet = environment_->frameSet(0);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, skyboxPipeLayout_, 3, 1, &envSet, 0,
                            nullptr);
    struct Push {
        glm::mat4 viewProj;
        glm::vec4 cameraPos;
    } push{};
    if (scene_) {
        glm::mat4 viewNoTrans = glm::mat4(glm::mat3(scene_->camera.view));
        push.viewProj = scene_->camera.proj * viewNoTrans;
        push.cameraPos = glm::vec4(scene_->camera.worldPosition, 1.0f);
    }
    vkCmdPushConstants(cb, skyboxPipeLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    VkBuffer vb = skyboxVerts_.buffer;
    VkDeviceSize off = 0;
    if (vb != VK_NULL_HANDLE && skyboxIndices_.buffer != VK_NULL_HANDLE) {
        vkCmdBindVertexBuffers(cb, 0, 1, &vb, &off);
        vkCmdBindIndexBuffer(cb, skyboxIndices_.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, 36, 1, 0, 0, 0);
    }
    vkCmdEndRendering(cb);
}

void SceneRenderer::recordTonemap(VkCommandBuffer cb, const FrameContext& ctx) {
    auto& g = activeGraph_->resources();
    const auto& hdr = g[refs_.hdr.id];
    lastHdrImage_ = hdr.image;
    lastHdrExtent_ = ctx.renderExtent;
    VkDescriptorImageInfo info{};
    info.sampler = gbufferSampler_;
    info.imageView = hdr.view;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = tonemapSets_[activeSlot_];
    w.dstBinding = 0;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.descriptorCount = 1;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = ctx.swapchainImageView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    beginTarget(cb, {color}, nullptr, ctx.renderExtent);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipeLayout_, 0, 1,
                            &tonemapSets_[activeSlot_], 0, nullptr);
    vkCmdPushConstants(cb, tonemapPipeLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(exposure_),
                       &exposure_);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

void SceneRenderer::buildPasses(RenderGraph& graph, const FrameContext& ctx) {
    activeGraph_ = &graph;
    refs_ = FrameGraphRefs{};

    auto swapchain = graph.importImage("SwapchainColor", ctx.swapchainImage, ctx.swapchainImageView,
                                       ctx.swapchainFormat, ctx.renderExtent, ResourceUsage::None);
    // Ping-pong history breaks the HiZ -> GBuffer -> copy -> history cycle:
    // this frame samples last frame's copy while writing the other slot.
    uint32_t readSlot = static_cast<uint32_t>(ctx.frameIndex % 2);
    uint32_t writeSlot = 1 - readSlot;
    auto historyRead = graph.importImage("HistoryDepthRead", historyImages_[readSlot],
                                         historyViews_[readSlot], VK_FORMAT_D32_SFLOAT,
                                         ctx.renderExtent, ResourceUsage::ShaderRead);
    auto historyWrite = graph.importImage("HistoryDepthWrite", historyImages_[writeSlot],
                                          historyViews_[writeSlot], VK_FORMAT_D32_SFLOAT,
                                          ctx.renderExtent, ResourceUsage::ShaderRead);
    auto lightBuf = graph.importBuffer("LightsSSBO", lightRings_[activeSlot_].buffer,
                                       kMaxLights * sizeof(GPULight), BufferUsage::ComputeRead);
    auto gridBuf = graph.importBuffer("ClusterGridSSBO", clusterGrid_[activeSlot_].buffer,
                                      kGridXCap * kGridYCap * kClusterZ * sizeof(ClusterCell),
                                      BufferUsage::None);
    auto indexBuf = graph.importBuffer("ClusterIndexSSBO", clusterIndex_[activeSlot_].buffer,
                                       sizeof(uint32_t) + kIndexListCap * sizeof(uint32_t),
                                       BufferUsage::None);
    auto meshletBuf = graph.importBuffer("MeshletsSSBO", meshletPool_.buffer,
                                         meshletPool_.size, BufferUsage::ComputeRead);
    auto uniqueBuf = graph.importBuffer("UniqueVertsSSBO", uniquePool_.buffer, uniquePool_.size,
                                        BufferUsage::ComputeRead);
    auto triBuf = graph.importBuffer("LocalTrisSSBO", triPool_.buffer, triPool_.size,
                                     BufferUsage::ComputeRead);
    auto compactedBuf = graph.importBuffer("CompactedIndices", compacted_[activeSlot_].buffer,
                                           kCompactedCap * sizeof(uint32_t), BufferUsage::None);
    auto indirectBuf = graph.importBuffer("IndirectDrawCommand", indirect_[activeSlot_].buffer,
                                          sizeof(VkDrawIndexedIndirectCommand), BufferUsage::None);
    auto residencyBuf = graph.importBuffer("ResidencyTable", residency_[activeSlot_].buffer,
                                           (kMaxPages + 1) * sizeof(uint32_t), BufferUsage::None);
    auto requestBuf = graph.importBuffer("PageRequests", requests_[activeSlot_].buffer,
                                         (kRequestCap + 1) * sizeof(uint32_t), BufferUsage::None);
    refs_.lightBuffer = lightBuf;
    refs_.clusterGrid = gridBuf;
    refs_.clusterIndex = indexBuf;
    refs_.meshletBuffer = meshletBuf;
    refs_.compacted = compactedBuf;
    refs_.indirect = indirectBuf;
    refs_.residency = residencyBuf;
    refs_.requests = requestBuf;

    // Shadow cascade depths (legacy-owned, graph-tracked for layout + lifetime).
    ResourceHandle cascadeViews[4];
    for (uint32_t c = 0; c < 4; ++c) {
        char name[32];
        std::snprintf(name, sizeof(name), "ShadowCascade%u", c);
        cascadeViews[c] = graph.importImage(name, VK_NULL_HANDLE, shadowPass_->view(c),
                                            VK_FORMAT_D32_SFLOAT, {1024, 1024}, ResourceUsage::None);
    }
    refs_.gbuffer = GBuffer::declare(graph, ctx.renderExtent);
    refs_.hizExtent = {std::max(1u, ctx.renderExtent.width / 2),
                       std::max(1u, ctx.renderExtent.height / 2)};
    refs_.hizMips = 1;
    for (uint32_t d = std::max(refs_.hizExtent.width, refs_.hizExtent.height); d > 1; d >>= 1) {
        if (refs_.hizMips >= 11) break;
        ++refs_.hizMips;
    }
    auto hizPyramid = graph.createResource({.name = "HiZ_Pyramid",
                                            .format = VK_FORMAT_R32_SFLOAT,
                                            .extent = refs_.hizExtent,
                                            .usage = VK_IMAGE_USAGE_STORAGE_BIT |
                                                     VK_IMAGE_USAGE_SAMPLED_BIT,
                                            .mipLevels = refs_.hizMips});
    auto hdrTarget = graph.createResource({.name = "HDR_Color",
                                           .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                           .extent = ctx.renderExtent,
                                           .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                    VK_IMAGE_USAGE_SAMPLED_BIT |
                                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT});
    refs_.hiz = hizPyramid;
    refs_.hdr = hdrTarget;
    refs_.historyDepth = historyWrite;
    refs_.historyRead = historyRead;

    graph.addPass("ShadowPass", QueueType::Graphics,
                  [&](RenderGraphBuilder& b) {
                      for (uint32_t c = 0; c < 4; ++c) b.write(cascadeViews[c], ResourceUsage::DepthStencilAttachment);
                  },
                  [&](VkCommandBuffer cb) { recordShadow(cb, ctx); });
    graph.addPass("ClusterLightCullPass", QueueType::AsyncCompute,
                  [&](RenderGraphBuilder& b) {
                      b.read(lightBuf, BufferUsage::ComputeRead);
                      b.write(gridBuf, BufferUsage::ComputeWrite);
                      b.write(indexBuf, BufferUsage::ComputeWrite);
                  },
                  [&](VkCommandBuffer cb) { recordClusterCull(cb, ctx); });
    graph.addPass("HiZBuildPass", QueueType::AsyncCompute,
                  [&](RenderGraphBuilder& b) {
                      b.read(historyRead, ResourceUsage::ShaderRead);
                      b.write(hizPyramid, ResourceUsage::ComputeWrite);
                  },
                  [&](VkCommandBuffer cb) { recordHiZBuild(cb, ctx); });
    graph.addPass("MeshletCullPass", QueueType::AsyncCompute,
                  [&](RenderGraphBuilder& b) {
                      b.read(hizPyramid, ResourceUsage::ShaderRead);
                      b.read(meshletBuf, BufferUsage::ComputeRead);
                      b.read(uniqueBuf, BufferUsage::ComputeRead);
                      b.read(triBuf, BufferUsage::ComputeRead);
                      b.read(residencyBuf, BufferUsage::ComputeRead);
                      b.write(compactedBuf, BufferUsage::ComputeWrite);
                      b.write(indirectBuf, BufferUsage::ComputeWrite);
                      b.write(requestBuf, BufferUsage::ComputeWrite);
                  },
                  [&](VkCommandBuffer cb) { recordMeshletCull(cb, ctx); });
    graph.addPass("GBufferPass", QueueType::Graphics,
                  [&](RenderGraphBuilder& b) {
                      b.read(compactedBuf, BufferUsage::IndexBuffer);
                      b.read(indirectBuf, BufferUsage::IndirectBuffer);
                      b.write(refs_.gbuffer.albedoAO, ResourceUsage::ColorAttachment);
                      b.write(refs_.gbuffer.normalRoughness, ResourceUsage::ColorAttachment);
                      b.write(refs_.gbuffer.metallicFlags, ResourceUsage::ColorAttachment);
                      b.write(refs_.gbuffer.depth, ResourceUsage::DepthStencilAttachment);
                  },
                  [&](VkCommandBuffer cb) { recordGBuffer(cb, ctx); });
    graph.addPass("HistoryUpdatePass", QueueType::Graphics,
                  [&](RenderGraphBuilder& b) {
                      b.read(refs_.gbuffer.depth, ResourceUsage::TransferSrc);
                      b.write(historyWrite, ResourceUsage::TransferDst);
                  },
                  [&](VkCommandBuffer cb) {
                      auto& g = activeGraph_->resources();
                      const auto& depth = g[refs_.gbuffer.depth.id];
                      const auto& hist = g[refs_.historyDepth.id];
                      VkImageCopy copy{};
                      copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                      copy.srcSubresource.layerCount = 1;
                      copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                      copy.dstSubresource.layerCount = 1;
                      copy.extent = {ctx.renderExtent.width, ctx.renderExtent.height, 1};
                      vkCmdCopyImage(cb, depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     hist.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                      VkImageMemoryBarrier bar{};
                      bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                      bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                      bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                      bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                      bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                      bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                      bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                      bar.image = hist.image;
                      bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                      bar.subresourceRange.levelCount = 1;
                      bar.subresourceRange.layerCount = 1;
                      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                           nullptr, 1, &bar);
                  });
    graph.addPass("DeferredLightingPass", QueueType::Graphics,
                  [&](RenderGraphBuilder& b) {
                      b.read(refs_.gbuffer.albedoAO, ResourceUsage::ShaderRead);
                      b.read(refs_.gbuffer.normalRoughness, ResourceUsage::ShaderRead);
                      b.read(refs_.gbuffer.metallicFlags, ResourceUsage::ShaderRead);
                      b.read(refs_.gbuffer.depth, ResourceUsage::ShaderRead);
                      b.read(cascadeViews[0], ResourceUsage::ShaderRead);
                      b.read(gridBuf, BufferUsage::FragmentRead);
                      b.read(indexBuf, BufferUsage::FragmentRead);
                      b.write(hdrTarget, ResourceUsage::ColorAttachment);
                  },
                  [&](VkCommandBuffer cb) { recordDeferred(cb, ctx); });
    graph.addPass("ForwardPass", QueueType::Graphics,
                  [&](RenderGraphBuilder& b) {
                      // Read-only depth test (LESS_EQUAL, no write); the record
                      // begins rendering with DEPTH_READ_ONLY_OPTIMAL to match.
                      b.read(refs_.gbuffer.depth, ResourceUsage::DepthStencilAttachment);
                      b.write(hdrTarget, ResourceUsage::ColorAttachment);
                  },
                  [&](VkCommandBuffer cb) { recordForward(cb, ctx); });
    graph.addPass("PostProcessPass", QueueType::Graphics,
                  [&](RenderGraphBuilder& b) {
                      b.read(hdrTarget, ResourceUsage::ShaderRead);
                      b.write(swapchain, ResourceUsage::ColorAttachment);
                  },
                  [&](VkCommandBuffer cb) { recordTonemap(cb, ctx); });
    graph.addPass("EditorOverlayPass", QueueType::Graphics,
                  [&](RenderGraphBuilder& b) { b.write(swapchain, ResourceUsage::ColorAttachment); },
                  [&](VkCommandBuffer cb) {
                      // Kept in Renderer (ImGui context owner); no-op here.
                      (void)cb;
                  });
    graph.addPass("PresentPass", QueueType::Graphics,
                  [&](RenderGraphBuilder& b) { b.read(swapchain, ResourceUsage::Present); },
                  [&](VkCommandBuffer) {});
}

void SceneRenderer::debugReadback(const char* path) {
    if (!path || lastHdrImage_ == VK_NULL_HANDLE || lastHdrExtent_.width == 0) return;
    vkDeviceWaitIdle(device_);
    const uint32_t w = lastHdrExtent_.width;
    const uint32_t h = lastHdrExtent_.height;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize bytes = VkDeviceSize(w) * h * 8; // R16G16B16A16F
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = bytes;
    ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &ci, nullptr, &buf) != VK_SUCCESS) return;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, buf, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        findMemoryType(physical_, req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device_, &ai, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buf, nullptr);
        return;
    }
    vkBindBufferMemory(device_, buf, mem, 0);
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = graphicsFamily_;
    if (vkCreateCommandPool(device_, &pci, nullptr, &pool) != VK_SUCCESS) {
        vkFreeMemory(device_, mem, nullptr);
        vkDestroyBuffer(device_, buf, nullptr);
        return;
    }
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo abi{};
    abi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    abi.commandPool = pool;
    abi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    abi.commandBufferCount = 1;
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    bool ok = vkAllocateCommandBuffers(device_, &abi, &cb) == VK_SUCCESS &&
              vkCreateFence(device_, &fi, nullptr, &fence) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier toSrc{};
        toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSrc.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image = lastHdrImage_;
        toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toSrc.subresourceRange.levelCount = 1;
        toSrc.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toSrc);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cb, lastHdrImage_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
                               &region);
        VkImageMemoryBarrier toRead = toSrc;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toRead);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        ok = vkQueueSubmit(graphicsQueue_, 1, &si, fence) == VK_SUCCESS &&
             vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    }
    if (ok) {
        void* mapped = nullptr;
        if (vkMapMemory(device_, mem, 0, bytes, 0, &mapped) == VK_SUCCESS && mapped) {
            // CPU tonemap (ACES approx + gamma) into PPM for inspection.
            FILE* f = std::fopen(path, "wb");
            if (f) {
                std::fprintf(f, "P6\n%u %u\n255\n", w, h);
                const uint16_t* px = static_cast<const uint16_t*>(mapped);
                auto halfToFloat = [](uint16_t v) {
                    uint32_t sign = (v >> 15) & 1, exp = (v >> 10) & 0x1f, mant = v & 0x3ff;
                    uint32_t f = 0;
                    if (exp == 0) f = sign << 31;
                    else if (exp == 31) f = (sign << 31) | 0x7f800000;
                    else f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
                    float out = 0.0f;
                    std::memcpy(&out, &f, 4);
                    return out;
                };
                for (uint32_t i = 0; i < w * h; ++i) {
                    unsigned char rgb[3];
                    for (int c = 0; c < 3; ++c) {
                        float hdr = halfToFloat(px[i * 4 + c]) * exposure_;
                        float aces =
                            std::min(1.0f, std::max(0.0f, (hdr * (2.51f * hdr + 0.03f)) /
                                                              (hdr * (2.43f * hdr + 0.59f) + 0.14f)));
                        rgb[c] = static_cast<unsigned char>(std::pow(aces, 1.0f / 2.2f) * 255.0f);
                    }
                    std::fwrite(rgb, 1, 3, f);
                }
                std::fclose(f);
            }
            vkUnmapMemory(device_, mem);
        }
    }
    if (fence != VK_NULL_HANDLE) vkDestroyFence(device_, fence, nullptr);
    vkDestroyCommandPool(device_, pool, nullptr);
    vkFreeMemory(device_, mem, nullptr);
    vkDestroyBuffer(device_, buf, nullptr);
}

void SceneRenderer::debugReadbackDepth(const char* path) {
    if (!path) return;
    vkDeviceWaitIdle(device_);
    VkImage src = historyImages_[0];
    VkExtent2D extent = historyExtent_;
    if (src == VK_NULL_HANDLE || extent.width == 0) return;
    const uint32_t w = extent.width;
    const uint32_t h = extent.height;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = bytes;
    ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &ci, nullptr, &buf) != VK_SUCCESS) return;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, buf, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        findMemoryType(physical_, req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device_, &ai, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buf, nullptr);
        return;
    }
    vkBindBufferMemory(device_, buf, mem, 0);
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = graphicsFamily_;
    if (vkCreateCommandPool(device_, &pci, nullptr, &pool) != VK_SUCCESS) {
        vkFreeMemory(device_, mem, nullptr);
        vkDestroyBuffer(device_, buf, nullptr);
        return;
    }
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo abi{};
    abi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    abi.commandPool = pool;
    abi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    abi.commandBufferCount = 1;
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    bool ok = vkAllocateCommandBuffers(device_, &abi, &cb) == VK_SUCCESS &&
              vkCreateFence(device_, &fi, nullptr, &fence) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier toSrc{};
        toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSrc.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image = src;
        toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        toSrc.subresourceRange.levelCount = 1;
        toSrc.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
        VkImageMemoryBarrier toRead = toSrc;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toRead);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        ok = vkQueueSubmit(graphicsQueue_, 1, &si, fence) == VK_SUCCESS &&
             vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    }
    if (ok) {
        void* mapped = nullptr;
        if (vkMapMemory(device_, mem, 0, bytes, 0, &mapped) == VK_SUCCESS && mapped) {
            FILE* f = std::fopen(path, "wb");
            if (f) {
                std::fprintf(f, "P6\n%u %u\n255\n", w, h);
                const float* px = static_cast<const float*>(mapped);
                float mn = 2.0f, mx = -1.0f;
                for (uint32_t i = 0; i < w * h; ++i) {
                    mn = std::min(mn, px[i]);
                    mx = std::max(mx, px[i]);
                }
                std::fprintf(stderr, "[readback] depth range min=%.4f max=%.4f\n", mn, mx);
                for (uint32_t i = 0; i < w * h; ++i) {
                    unsigned char v = static_cast<unsigned char>(
                        std::min(1.0f, std::max(0.0f, px[i])) * 255.0f);
                    unsigned char rgb[3] = {v, v, v};
                    std::fwrite(rgb, 1, 3, f);
                }
                std::fclose(f);
            }
            vkUnmapMemory(device_, mem);
        }
    }
    if (fence != VK_NULL_HANDLE) vkDestroyFence(device_, fence, nullptr);
    vkDestroyCommandPool(device_, pool, nullptr);
    vkFreeMemory(device_, mem, nullptr);
    vkDestroyBuffer(device_, buf, nullptr);
}
} // namespace Engine
