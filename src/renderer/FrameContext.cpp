#include "renderer/FrameContext.hpp"
#include <glm/gtc/matrix_inverse.hpp>

namespace Engine {

// Helper to populate camera snapshot from view/proj
CameraSnapshot makeCameraSnapshot(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& worldPos, float zNear, float zFar, float fov, float aspect) {
    CameraSnapshot snap;
    snap.viewMatrix = view;
    snap.projMatrix = proj;
    snap.invViewProj = glm::inverse(proj * view);
    snap.worldPosition = worldPos;
    snap.zNear = zNear;
    snap.zFar = zFar;
    snap.fov = fov;
    snap.aspectRatio = aspect;
    return snap;
}

} // namespace Engine
