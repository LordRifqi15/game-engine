#pragma once
#include <glm/glm.hpp>
#include <cstdint>

namespace Engine {

enum class SamplerType : uint32_t {
    LinearRepeat = 0,
    LinearClamp = 1,
    NearestRepeat = 2,
    NearestClamp = 3,
    ShadowCompare = 4,
    Count = 5
};

// 64-byte std430 aligned structure
struct alignas(16) GPUMaterial {
    uint32_t albedoTextureID{0};             // Index into globalTextures[]
    uint32_t normalTextureID{1};             // Default flat normal [0.5, 0.5, 1.0]
    uint32_t metallicRoughnessTextureID{2};  // Default black/neutral
    uint32_t samplerID{0};                   // Index into globalSamplers[]

    glm::vec4 baseColorFactor{1.0f};
    float metallicFactor{1.0f};
    float roughnessFactor{1.0f};
    glm::vec2 padding{0.0f};
};

static_assert(sizeof(GPUMaterial) == 48, "GPUMaterial must be 48 bytes std430");
static_assert(alignof(GPUMaterial) == 16, "GPUMaterial align 16");

} // namespace Engine

namespace engine {
    using SamplerType = Engine::SamplerType;
    using GPUMaterial = Engine::GPUMaterial;
}
