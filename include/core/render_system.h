#pragma once

#include "core/bounds_component.h"
#include "core/instance_data.h"
#include "core/job_system.h"
#include "core/hiz_buffer.h"
#include "core/material_component.h"
#include "core/occlusion_component.h"
#include "core/mesh_component.h"
#include "core/frustum.h"
#include "core/registry.h"
#include "core/transform_component.h"
#include "renderer/renderer.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

// Batches renderable entities by mesh, frustum-culls before submission.
// Culling + instance building run on the job system; submission stays single-threaded.
namespace engine {

class RenderSystem {
public:
    explicit RenderSystem(Renderer& renderer, JobSystem& jobs)
        : renderer_(renderer), jobs_(jobs) {}

    void beginFrame(const Camera& camera, const DirectionalLight& light) {
        renderer_.beginFrame(camera, light);
        Camera cam = camera;
        if (renderer_.swapchainHeight() > 0) {
            cam.aspect = static_cast<float>(renderer_.swapchainWidth()) /
                         static_cast<float>(renderer_.swapchainHeight());
        }
        lastViewProjection_ = cam.viewProjection();
        lastCameraPos_ = cam.position;
    }

    void update(Registry& registry) {
        auto& transforms = registry.array<TransformComponent>();
        auto& meshes = registry.array<MeshComponent>();
        auto& materials = registry.array<MaterialComponent>();
        auto* bounds = registry.tryGetComponentArray<BoundsComponent>();
        auto* occlusion = registry.tryGetComponentArray<OcclusionComponent>();

        Frustum frustum = frustumForCamera();

        // Per-thread local batches -> merged after (no locks in the hot loop).
        const size_t workerCount = jobs_.threadCount();
        perThreadBatches_.assign(workerCount, {});
        perThreadOrder_.assign(workerCount, {});

        const size_t entityCount = meshes.size();
        parallel_for(jobs_, entityCount, [&](size_t begin, size_t end) {
            const size_t slot = std::min(begin / chunkSize(entityCount, workerCount),
                                         workerCount - 1);
            auto& local = perThreadBatches_[slot];
            for (size_t i = begin; i < end; ++i) {
                Entity e = meshes.entityAt(i);
                if (!transforms.has(e) || !materials.has(e)) continue;
                if (!meshes.get(e).mesh) continue;

                const TransformComponent& tc = transforms.get(e);
                // Pass 1: frustum cull.
                if (bounds && bounds->has(e)) {
                    const BoundsComponent& b = bounds->get(e);
                    float scaleRadius = tc.scale.x;
                    if (tc.scale.y > scaleRadius) scaleRadius = tc.scale.y;
                    if (tc.scale.z > scaleRadius) scaleRadius = tc.scale.z;
                    if (!frustum.intersectsSphere(tc.position + b.center, b.radius * scaleRadius)) {
                        ++culledCount_;
                        continue;
                    }
                }

                // Pass 2: Hi-Z occlusion (previous frame's depth). Requires bounds
                // component; conservative — never culls a visible object.
                if (bounds && bounds->has(e) && occlusion) {
                    const BoundsComponent& b = bounds->get(e);
                    glm::vec4 center(tc.position + b.center, 1.0f);
                    glm::vec4 clip = lastViewProjection_ * center;
                    if (clip.w > 0.0f) {
                        glm::vec3 ndc = glm::vec3(clip) / clip.w;
                        float screenX = (ndc.x * 0.5f + 0.5f) * renderer_.swapchainWidth();
                        float screenY = (ndc.y * -0.5f + 0.5f) * renderer_.swapchainHeight();
                        float radiusPx = b.radius / clip.w *
                                         static_cast<float>(renderer_.swapchainHeight()) /
                                         (2.0f * std::tan(glm::radians(camFov_) * 0.5f));
                        float nearestDepth = std::clamp(ndc.z - b.radius / clip.w, 0.0f, 1.0f);

                        if (hiz_.isOccluded(screenX - radiusPx, screenY - radiusPx,
                                            screenX + radiusPx, screenY + radiusPx,
                                            nearestDepth,
                                            renderer_.swapchainWidth(),
                                            renderer_.swapchainHeight())) {
                            ++occludedCount_;
                            continue;
                        }
                    }
                }

                InstanceData inst;
                glm::mat4 model = glm::mat4(1.0f);
                model = glm::translate(model, tc.position);
                model *= glm::mat4_cast(glm::quat(tc.rotation));
                model = glm::scale(model, tc.scale);
                inst.model = model;
                inst.color = materials.get(e).material.baseColor;
                inst.params = {materials.get(e).material.metallic,
                               materials.get(e).material.roughness, 0.0f, 0.0f};
                const Texture* tex = materials.get(e).material.baseColorTexture;
                uint64_t key = reinterpret_cast<uint64_t>(meshes.get(e).mesh) ^
                               (reinterpret_cast<uint64_t>(tex) >> 1);
                if (!local.count(key)) local[key]; // ensure entry
                auto& entry = local[key];
                if (entry.empty()) perThreadOrder_[slot][key] = {meshes.get(e).mesh, tex};
                entry.push_back(inst);
            }
        });

        // Merge thread-local batches in deterministic order.
        batches_.clear();
        batchOrder_.clear();
        for (size_t t = 0; t < perThreadBatches_.size(); ++t) {
            for (auto& [key, list] : perThreadBatches_[t]) {
                auto it = perThreadOrder_[t].find(key);
                if (it == perThreadOrder_[t].end()) continue;
                if (batches_.find(key) == batches_.end())
                    batchOrder_.emplace_back(it->second.first, it->second.second);
                auto& dst = batches_[key];
                dst.insert(dst.end(), list.begin(), list.end());
            }
        }
        for (auto& [mesh, tex] : batchOrder_) {
            uint64_t key = reinterpret_cast<uint64_t>(mesh) ^ (reinterpret_cast<uint64_t>(tex) >> 1);
            renderer_.drawMeshInstanced(*mesh, batches_[key], tex);
        }
    }

    void endFrame() { renderer_.endFrame(); }

    void setCamera(const Camera& camera) { camera_ = camera; camFov_ = camera.fov; }

    // Builds CPU Hi-Z from the previous frame's readback depth.
    void buildHiZ(const std::vector<float>& depth, uint32_t w, uint32_t h) {
        hiz_.build(depth, w, h);
    }
    uint32_t occludedLastFrame() const { return occludedCount_; }
    uint32_t culledLastFrame() const { return culledCount_; }

private:
    static size_t chunkSize(size_t count, size_t workers) {
        return count < workers ? count : (count + workers - 1) / workers;
    }

    Frustum frustumForCamera() const {
        Camera cam = camera_;
        if (renderer_.swapchainHeight() > 0) {
            cam.aspect = static_cast<float>(renderer_.swapchainWidth()) /
                         static_cast<float>(renderer_.swapchainHeight());
        }
        return Frustum::fromViewProjection(cam.viewProjection());
    }

    Renderer& renderer_;
    JobSystem& jobs_;
    Camera camera_{};
    float camFov_ = 70.0f;
    glm::mat4 lastViewProjection_{1.0f};
    glm::vec3 lastCameraPos_{0.0f, 0.0f, 3.0f};
    HiZBuffer hiz_;
    uint32_t culledCount_ = 0;
    uint32_t occludedCount_ = 0;
    // Key = mesh ptr XOR texture ptr>>1; order list keeps deterministic submission.
    std::unordered_map<uint64_t, std::vector<InstanceData>> batches_;
    std::vector<std::pair<Mesh*, const Texture*>> batchOrder_;
    std::vector<std::unordered_map<uint64_t, std::pair<Mesh*, const Texture*>>> perThreadOrder_;
    std::vector<std::unordered_map<uint64_t, std::vector<InstanceData>>> perThreadBatches_;
};

} // namespace engine
