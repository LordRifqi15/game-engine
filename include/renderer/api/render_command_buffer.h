#pragma once

#include "core/instance_data.h"
#include "core/light.h"
#include "core/mesh.h"

#include <cstdint>
#include <vector>

// Backend-neutral command buffer interface. No Vulkan types allowed here.
namespace engine {

// One mesh + one texture + its instance list = one instanced draw call.
class Texture;

struct PendingBatch {
    const Mesh* mesh;
    const Texture* texture;
    uint32_t firstInstance; // offset into flat instance buffer
    uint32_t instanceCount;
};

class RenderCommandBuffer {
public:
    virtual ~RenderCommandBuffer() = default;

    // Resets this frame's buffer, uploads instance data, records one instanced
    // draw per batch for imageIndex. viewProjection/light go to the frame UBO.
    virtual void recordFrame(uint32_t frameIndex, uint32_t imageIndex,
                             const std::vector<PendingBatch>& batches,
                             const glm::mat4& viewProjection,
                             const DirectionalLight& light,
                             const glm::vec3& cameraPos) = 0;
};

} // namespace engine
